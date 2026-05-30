# Networking Architecture — Design (Consolidated)

**Status:** Approved architecture; phased implementation to follow.
**Date:** 2026-05-30
**Scope:** Engine-level networking foundation + the contract games build on. This is the *umbrella* design. Each implementation phase has its own context spec (see **Phasing** at the end), and each of those gets its own `writing-plans` implementation plan.

---

## 1. Goal

Give the engine a networking foundation that is **game-agnostic** — the engine deals only in abstractions and owns the IO threading — while each game owns its connection topology, protocol, and per-stage connection logic. The first consumer is the in-house ARPG (PoE2-style, server-authoritative): a client that holds **two** outbound connections (auth server + world server), with the ability to spin up a **local dedicated server** in-editor for prototyping.

## 2. Guiding principle: mechanism vs. policy

| Layer | Owns | Knows about |
|-------|------|-------------|
| **netlib** (new standalone lib) | Transport abstractions + a concrete TCP impl + an in-memory impl + framing | Bytes, sockets, framing. *Nothing* about games. |
| **Engine** | io-object registry, the event **sink** + lock-free MPSC ring + `NetBufferPool`, the `NetServices` bridge, hot-reload teardown timing. Owns **no** IO threads. | The *abstract* interfaces only. Never names "TCP". |
| **Game** (`Game.dll`) | How many connections, what each connects to, protocol/opcode meaning, the connecting→authed→in-world stage machine | Everything game-specific. Picks which impl to use. |

This is **dependency inversion / ports-and-adapters**: the engine is the port consumer, a transport impl is an adapter, the game wires the adapter in. It mirrors the existing **`NavServices`** bridge (engine owns `NavMeshSystem`; `Game.dll` calls a function-pointer table threaded through `SystemContext`, with zero link dependency on `Engine.dll`).

## 3. Layering & dependency graph

```
netlib.dll  (NEW — standalone, no engine dependency)
   ├─ interfaces:  IIoClient / IIoServer / byte-stream + framing contracts   ← pure abstractions
   └─ impls:       TcpClient / TcpServer (length-prefix framing), InMemory adapter (loopback)
Engine.dll  ─depends on→ netlib INTERFACES ONLY  (never the concrete impls; never the token "TCP")
Game.dll    ─depends on→ netlib (to pick/instantiate impls) + Engine (only via the NetServices bridge)
editor.exe / runtime.exe / server.exe  ─link→ Engine + ecs + netlib
```

`netlib` is built as a `SHARED` library exactly like `ecs.dll` / `Engine.dll`: `add_library(netlib SHARED ...)`, a `NETLIB_EXPORTS` compile define, and a `NETLIB_API` (`__declspec(dllexport/import)`) macro guarding its public symbols. Output to `RUNTIME_DIR`, `FOLDER Libraries`, MSVC embedded debug info — copy `src/ecs/CMakeLists.txt`.

**Why the engine depends only on interfaces:** a game can supply its own impl (UDP, a mock, a record/replay adapter) and the engine is none the wiser. The TCP and in-memory impls `netlib` ships are a convenience, not a requirement.

## 4. Threading & data flow — **push/sink, adapter-owned threading**

The contract is **push, not pull**. The transport adapter owns its own threading and *pushes* events into an engine-provided thread-safe sink; the engine does **not** poll the adapter and does **not** own an IO pump thread. This is required to host **Windows IOCP** for the server: IOCP associates sockets with a completion port and runs N worker threads that block on `GetQueuedCompletionStatus`, waking only when the kernel completes an overlapped `WSARecv`/`WSASend`. IOCP *must* own its threads — a `Poll()` pull contract cannot express it. (See §0-rationale in the Phase-1 spec.)

- **TcpServer** = IOCP: completion port + worker pool, overlapped `WSARecv`/`WSASend`, framing on the completing worker, scales to thousands of connections.
- **TcpClient** = async non-blocking (overlapped I/O / event-driven), **no busy-spin**. IOCP-single-worker is an acceptable impl, but not required (the game ARPG client holds only 2 connections — auth + world — so IOCP's scale advantage is irrelevant; low latency is what matters).
- **In-memory** adapter delivers via the same sink (synchronous on the caller thread in tests for determinism).

The **game never owns threads** — that intent is preserved; transport threads live in `netlib` (a stable lib) or a game's stable adapter DLL, never in game gameplay code.

```
        OS socket(s)                         netlib adapter (owns its threads)
            │                          ┌───────────────────────────────────────┐
   IOCP completion port  ◄────────────┤  TcpServer: IOCP worker pool            │
            │                          │   • WSARecv completes on a worker       │
            ▼  (many worker threads)   │   • FRAME here (length-prefix reassembly)│
   GetQueuedCompletionStatus  ─────────┤   • sink->OnIoEvent(evt)  ◄── MANY threads│
                                       └───────────────────┬─────────────────────┘
                                                           │  IIoSink::OnIoEvent (thread-safe!)
   ┌───────────────────────────────────────────────────────▼──────────────────┐
   │  Engine sink (IIoSink impl)                                                │
   │   • copy borrowed payload → NetBufferPool (lock-free, on the adapter thread)│
   │   • enqueue NetEvent → lock-free bounded MPSC ring  (MANY producers)        │
   └───────────────────────────────────────────────────────┬──────────────────┘
                                                            │  MpscRing<NetEvent,N> (Vyukov)
   ┌────────────────────────────────────────────────────────▼─────────────────┐
   │  GameThread (single consumer)                                              │
   │   • game NetSystem drains the MPSC ring via NetServices::PollEvent          │
   │   • stage machine + opcode dispatch + ECS mutation                          │
   │   • NetServices::Send → adapter.Send() DIRECTLY (thread-safe; posts WSASend) │
   └────────────────────────────────────────────────────────────────────────────┘
```

### Where framing lives — unchanged
Framing stays **in the adapter, on the adapter's thread** (the IOCP worker, the client's I/O thread). The TCP adapter does length-prefix reassembly (read a `uint32` length, then exactly that many bytes) as each overlapped recv completes. Engine stays byte-agnostic; the game still owns what bytes *mean* (opcode semantics) on GameThread.

### Crossing the thread boundary — lock-free MPSC inbound, direct outbound
- **Inbound is MPSC**, not per-connection SPSC: many adapter threads (IOCP workers + the client thread) produce; one GameThread consumes. Use a **lock-free bounded MPSC ring** (Vyukov bounded-MPMC: per-cell sequence counter + CAS on the enqueue position; fixed array, no per-op allocation, natural backpressure when full). New primitive `MpscRing.h`, beside `SpscRing.h`. The old per-connection SPSC rings are dropped.
- **Payloads are pooled, lock-free.** A `NetEvent` carries `{kind, opcode, conn, ptr, len}` (POD). The bytes live in a **lock-free `NetBufferPool`** (fixed-block, atomic free-list) — adapter threads acquire+`memcpy` inside `OnIoEvent`, GameThread releases after consuming. (`SpscRing`/`MpscRing` store `T` by value, so variable-length payloads can never be inline — same reason `RendererCommand` uses `StagingBufferPool`.)
- **Outbound needs no ring.** `NetServices::Send` calls `adapter.Send()` directly from GameThread; `Send` is thread-safe (posts `WSASend`, serializes per-connection internally). The Game→IO ring is gone.
- **Backpressure** (MPSC ring full / pool exhausted): `SM_WARN` + policy decided in Phase 2 (never silent — `feedback_logging_over_silent_skip`).

### Perf knobs (real-time MP — squeeze latency)
`TCP_NODELAY` **on by default** (Nagle batches small sends ~40ms — unacceptable for real-time), configurable per connection; `SO_REUSEADDR`; configurable send/recv buffer sizes. Documented in Phase 1.

## 5. The inbound unit: `NetEvent` (tagged), not raw bytes

The adapter's thread (IOCP worker / client I/O thread) pushes a small **tagged** record — via the engine sink — onto the lock-free MPSC ring. The engine is never "raw bytes" *or* a single fixed message rule; it just moves opaque tagged events. `NetEvent` is POD (it must be, to live by-value in the ring; the payload bytes are pooled separately):

```cpp
struct NetEvent {
    enum class Kind : uint8_t { Connected, Disconnected, Error, Message };
    Kind      kind;
    uint16_t  opcode;    // valid when kind == Message
    NetHandle conn;      // which connection (servers fan out to many)
    uint8_t*  payload;   // NetBufferPool block; null for non-Message
    uint32_t  len;       // payload length
};
```

A **game system** (registered in `SystemScheduler`, runs on GameThread) drains these. *That system is the connection stage machine* — connecting→authed→in-world→dropped is just game code reacting to `Connected` / `Message` / `Disconnected` / `Error`. The engine surfaces lifecycle; the game decides what each stage *does*. This answers "how does the game decide what happens at each connection stage" — it's 100% game policy on GameThread, no engine involvement in semantics.

## 6. Ownership & hot-reload — **Model B (residence encodes behavior)**

### The platform fact that drives everything
A polymorphic C++ object carries a hidden vtable pointer into the DLL that **compiled** it. Every virtual call — *and the destructor* — dispatches through that pointer. `FreeLibrary` unmaps that code segment. So the instant `Game.dll` reloads, any call on a `Game.dll`-compiled object (including `delete`) jumps into unmapped memory → crash.

**Ownership (who calls `delete`) and survival (whose code segment holds the vtable) are different axes.** Moving a `unique_ptr` to the engine does **not** relocate the code. Therefore a `Game.dll`-authored impl **cannot** outlive a `Game.dll` reload, regardless of who owns it. This is a constraint, not a choice.

### How Model B turns that into a feature
The *residence* of the adapter naturally encodes the reload behavior, matching the two real workflows:

| Adapter compiled into… | On `Game.dll` reload | Use case |
|------------------------|----------------------|----------|
| **stable binary** (`netlib`, or a game's own stable adapter DLL) | connection **survives** | "tweak UI / gameplay, keep my world connection alive" |
| **`Game.dll`** itself | connection **force-released** before `FreeLibrary` | "I'm editing the protocol/adapter — dropping the connection is fine and expected" |

In both cases the **engine still knows nothing about which impl it runs** — it holds an abstract `IIoClient*` / `IIoServer*` and never downcasts.

### Wiring & ownership mechanics
- The game wires an adapter by handing the engine a **factory** (a function that constructs an `IIo*`). *Which DLL the factory lives in* sets the behavior automatically: `netlib::MakeTcpClient` (stable) → survives; a `Game.dll`-local factory → `Game.dll`-bound.
- The engine owns the **lifecycle slot**: the io-object registry, the sink, and teardown timing. The object's *code* (and the threads it spawned) must reside in a DLL alive as long as the engine holds it.
- `netlib` adapters: always loaded → engine holds them (and their IOCP/client threads) across reloads freely.
- `Game.dll` adapters: need a **pre-reload teardown hook** — the networking analog of today's `SystemScheduler::Clear()`.

### Where the teardown hook slots (verified)
`GameLibrary::LoadOrReload` (`src/engine/src/threading/GameLibrary.cpp:87-95`) on reload runs, in order: `GameExit(state)` → `m_Scheduler->Clear()` (destroys `Game.dll` `ISystem`s while vtables mapped) → `FreeLibrary` → swap → `GameRegisterSystems`. The Net teardown must run in that same pre-`FreeLibrary` window. With the push/sink model the quiesce is **clean**: calling the adapter's **`Stop()` joins its worker threads** — after `Stop()` returns, no adapter thread is running, so no thread can be inside the about-to-be-unmapped `Game.dll` code. `Stop()` itself (and the dtor) live in the adapter's DLL, so they must run *before* `FreeLibrary` — which is exactly the hook window. Two viable placements (chosen in the Phase-2 spec):
  1. `GameLibrary` gains an `OnBeforeUnload` callback (like `SetScheduler`), invoked before `Clear()`.
  2. `GameThread`, on draining `m_ReloadPending`, calls the Net subsystem's pre-reload teardown *before* `m_GameLib.LoadOrReload(...)`.

## 7. Game-facing API: the `NetServices` bridge

Mirror `NavServices` exactly. The engine populates a `NetServices` function-pointer table at startup (`NetServicesImpl::Init`, analogous to `NavServicesImpl::Init`); GameThread threads a `const NetServices*` through `SystemContext` next to `Nav`. **Append-only ABI** (reordering/removing fields shifts pointer offsets and breaks `Game.dll` binary compat — same rule documented on `NavServices`). This keeps `Game.dll`'s link graph free of `Engine.dll`.

```cpp
struct NetServices {
    // Create/destroy connections. `factory` constructs the concrete IIo*; its
    // residence (stable DLL vs Game.dll) decides hot-reload survival (§6).
    NetHandle (*CreateClient)(IoClientFactory factory, const ClientConfig& cfg);
    NetHandle (*CreateServer)(IoServerFactory factory, const ServerConfig& cfg);
    void      (*Close)(NetHandle);

    // Drain the lock-free MPSC inbound ring (returns false when empty). Drains
    // ALL connections' events from one ring; `out->conn` says which. The payload
    // block is owned by the NetBufferPool and released back when GameThread is
    // done with it (PollEvent hands ownership to the caller for this event).
    bool      (*PollEvent)(NetEvent* out);

    // Send directly into the adapter (thread-safe; posts WSASend / queues).
    // Copies `data` before returning. False on backpressure.
    bool      (*Send)(NetHandle, uint16_t opcode, const uint8_t* data, size_t len);
};
```

`SystemContext` grows one field: `const NetServices* Net = nullptr;` (alongside `const NavServices* Nav`), set every tick in `GameThread::RunLoop`.

## 8. Dedicated server

A dedicated server = a **headless server-role game instance**: `Engine` core + `Game.dll` + its own `ECS` + `SystemScheduler` + an io-*server* adapter, **no renderer/overlay/window**. Wrap it as one concept — call it `ServerHost`, owning `{ECS, SystemScheduler, io-server}`. One concept, two residences:

- **Out-of-process** (Phase 3, ship first): a separate **`server.exe`** (a `runtime.exe` sibling — links `Engine` + `ecs` + `netlib`, loads `Game.dll`, but boots headless). The editor launches it; the client connects over **loopback TCP**. Production-shaped (same socket path real deployment uses → auth/replication bugs surface honestly), fully isolated (server crash or its own hot-reload doesn't touch the editor), one ECS world per process (sidesteps the singleton problem below).
- **In-process** (Phase 4, additive, gated): the editor instantiates a `ServerHost` beside its client world, behind an **in-memory** adapter (no real socket). Instant click-to-spawn, single debugger across both. **Blocked on de-singletoning** — see §9.

**The transport between client and local server is just another `IIo` adapter** — TCP-loopback for out-of-process, in-memory for in-process. The same game/server code runs behind either, so the engine never hard-picks a model; it only needs to (a) run a server-role instance and (b) expose the io-server abstraction.

### Headless boot is a real change
`Application::Init` currently *always* constructs `RenderThread` (and `PlatformThread`, which owns the GLFW window) — see `Application.cpp:51-62`. A true headless `server.exe` needs a boot path that runs GameThread + the Net IO pool **without** RenderThread/PlatformThread/NVRHI/GLFW. Phase 3 must add either an `Application` headless mode or a dedicated server bootstrap. This is the largest engine-structural item in Phase 3 and is called out in its spec.

## 9. The in-process blocker: process-global singletons

In-process dual-world (Phase 4) is gated on de-singletoning. `NavMeshSystem::Instance()` is a process-global singleton today (GameThread-only contract); `MeshSystem` / `MaterialSystem`, logging, and the alloc tracker very likely assume **one world per process**. Two in-process `ServerHost`s sharing a singleton `NavMeshSystem` would corrupt each other's nav state. Out-of-process gives each process its own singletons for free, which is exactly why it ships first. Phase 4 is a separate effort that must partition these subsystems per-world before in-process hosting is safe; it is explicitly **out of scope** for Phases 1–3.

## 10. Invariants (must hold across all phases)

1. **Engine never names a transport.** It manipulates `IIoClient*` / `IIoServer*` and never downcasts to TCP.
2. **`Game.dll` never links `Engine.dll`.** All engine networking reaches the game through the `NetServices` bridge; the game reaches `netlib` directly to pick impls.
3. **Framing lives in the adapter, on the adapter's thread.** Never in the engine; never in GameThread policy.
4. **The *game* owns no threads; transport threading is the adapter's concern.** The adapter (in `netlib` or a stable game adapter DLL) owns its IOCP/client threads and *pushes* events into the engine sink. The engine owns no IO pump thread.
5. **Adapter residence decides reload survival** (Model B). `Game.dll`-resident adapters are `Stop()`-joined (threads quiesced) + destroyed before every `FreeLibrary`.
6. **`NetServices` is append-only.** Same ABI discipline as `NavServices`.
7. **Inbound is one lock-free bounded MPSC ring; payloads are pooled, never inline.** Many adapter threads produce, GameThread consumes; `NetEvent` is POD with a `NetBufferPool` `{ptr,len}` (the `StagingBufferPool` pattern). Outbound is a direct thread-safe `Send()`, no ring.
8. **Sink callbacks must be thread-safe and fast.** `IIoSink::OnIoEvent` runs on adapter threads (many, for IOCP); it copies into the pool + enqueues, nothing heavy.

## 11. Phasing

Each phase produces working, independently-testable software and has its own **context spec** (this doc is the umbrella). Implementation plans are written per phase via `writing-plans`.

| Phase | Deliverable | Proof of done | Context spec |
|-------|-------------|---------------|--------------|
| **1. netlib standalone** | Interfaces (`IIoClient`/`IIoServer`/stream/framing) + TCP impl (with length-prefix framing) + in-memory adapter. Zero engine dependency. | Unit tests: framing reassembly (partial reads, coalesced reads), in-memory loopback round-trip, TCP loopback round-trip. | `2026-05-30-netlib-standalone-design.md` |
| **2. Engine integration** | io-object registry, engine `IIoSink`, lock-free MPSC ring + `NetBufferPool`, `NetServices` bridge (+ `SystemContext.Net`), Model-B teardown via adapter `Stop()`. No engine IO pump thread. | **Loopback round-trip**: a game `NetSystem` sends a message client↔server in one process via the in-memory adapter and observes it on GameThread; survives a `Game.dll` hot-reload when the adapter is `netlib`-resident. | `2026-05-30-net-engine-integration-design.md` |
| **3. Dedicated server (out-of-process)** | Headless boot path + `server.exe` target + editor spawn/supervise control + loopback-TCP client↔server wiring. | Editor launches `server.exe`; the in-editor client connects over loopback TCP and exchanges a message round-trip. | `2026-05-30-dedicated-server-out-of-process-design.md` |
| **4. In-process server (additive)** | `ServerHost` instantiated in-editor behind an in-memory adapter — **after** de-singletoning `NavMeshSystem` et al. | Deferred. Documented as a stub; not planned until Phase 3 lands and the singleton work is scoped. | (later) |

**Phase 1 is the dependency root** — everything else builds on its abstractions. Phases 3–4 get their plans when reached; Phase 4 additionally depends on a separate de-singletoning effort.

## 12. Open questions deferred to per-phase specs (not architecture-level)

- IOCP worker-pool sizing for the server adapter (threads = cores? fixed small N to start?) — perf decision, Phase 1/2.
- Backpressure policy on MPSC-ring-full / pool-exhaustion (stop-recv for TCP flow-control vs. drop) — Phase 2.
- Exact `ClientConfig` / `ServerConfig` shape (target address, listen port, buffer sizes, `noDelay`, max connections) — Phases 1–2.
- `NetHandle` representation (index+salt like mesh handles vs. opaque pointer) — Phase 2.
- `server.exe` ↔ editor supervision protocol (process spawn, health, shutdown signal) — Phase 3.

## 13. Cross-cutting requirements (all phases)

- **Logging is a primary debugging tool, not optional.** Networking failures are opaque without it: log connection lifecycle, every Winsock failure with its `WSAGetLastError()` code, framer anomalies, backpressure/drops (`SM_WARN`), and `Start`/`Stop`/teardown begin+end. Tests run verbose so a failure carries its full event trail. (`feedback_logging_over_silent_skip`.)
- **OS prompts are user-owned manual gates.** The first `listen()` (in `test_netlib`, the engine TCP test, and `server.exe`) may raise the **Windows Firewall allow-dialog**; some setups need **UAC** elevation. These cannot be auto-accepted — each phase's plan must include an explicit **"USER ACTION"** step at the moment they appear, and code must tolerate the pre-acceptance window (time out + log, never hang). Pure in-memory / lock-free-structure tests need no permission and run unattended.
