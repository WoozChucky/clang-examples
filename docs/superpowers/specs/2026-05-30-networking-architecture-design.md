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
| **Engine** | IO thread(s), io-object registry + pump, the ring plumbing, the `NetServices` bridge, hot-reload teardown timing | The *abstract* interfaces only. Never names "TCP". |
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

## 4. Threading & data flow

The engine already runs three coordinated threads (`Application` constructs PlatformThread + GameThread + RenderThread; see `src/engine/src/core/Application.{h,cpp}`). Networking adds **engine-owned IO thread(s)** — a small pool, sized N — *separate* from those three. The game never spawns threads; it asks the engine for capacity.

```
                 OS socket
                    │  (bytes)
   ┌────────────────▼─────────────────┐
   │  IO thread (engine-owned)         │   adapter->Poll()
   │  ── runs the adapter ──           │
   │  • recv bytes                     │
   │  • FRAME here (transport-specific)│ ← TCP impl: length-prefix reassembly
   │  • push NetEvent into inbound ring│   in-mem impl: hand up whole blobs
   └────────────────┬──────────────────┘
                    │  SpscRing<NetEvent,N>  (IO → Game)
   ┌────────────────▼──────────────────┐
   │  GameThread                        │
   │  • game NetSystem drains inbound   │ ← stage machine + opcode dispatch + ECS mutation
   │  • enqueues outbound messages      │
   └────────────────┬──────────────────┘
                    │  SpscRing<OutMsg,N>   (Game → IO)
                    ▼  adapter sends on its IO thread
```

### Where framing lives
Framing (turning a byte stream into discrete messages) is **transport-specific** and runs **in the adapter, on the IO thread** — *not* in the engine and *not* in GameThread policy:

- The TCP adapter does length-prefix reassembly (read a `uint32` length, then exactly that many bytes), solving partial reads at the point bytes arrive.
- A UDP adapter would surface whole datagrams. A custom adapter frames however it likes.

Consequences (all desirable):
- The engine stays genuinely byte-agnostic (hard requirement).
- Partial-read / reassembly pain is solved once, in the TCP adapter, reused by every game that picks it.
- GameThread doesn't burn tick budget scanning bytes — it receives whole messages.
- The game still owns **what bytes mean** (opcode semantics) — that's GameThread policy, separate from framing.

### Crossing the thread boundary
Reuse the existing lock-free idiom: per connection, one **inbound** `SpscRing` (IO→Game) and one **outbound** `SpscRing` (Game→IO). `src/common/include/SpscRing.h` is single-producer/single-consumer, power-of-two capacity — the IO-thread↔GameThread pairing fits exactly.

**Critical ring constraint (verified in `SpscRing.h`):** the ring stores `T` **by value** (`data[h & mask] = v`). Variable-length payloads therefore **cannot** live inline in the ring. The codebase's established answer is `RendererCommand` + `StagingBufferPool`: acquire a pooled buffer, `memcpy` the bytes in, put a raw pointer + length in the ring, the consumer returns the buffer to the pool after use (see `GameThread.cpp` mesh-upload path). Net payloads use the **same pattern** — a `NetBufferPool` owned engine-side, payload bytes pooled, the `NetEvent`/`OutMsg` in the ring carries `{opcode, ptr, len}` (POD). Backpressure = pool exhaustion / ring-full → `SM_WARN` + retry-or-drop policy (decided per phase).

## 5. The inbound unit: `NetEvent` (tagged), not raw bytes

The adapter pushes a small **tagged** record into the inbound ring — the engine is never "raw bytes" *or* a single fixed message rule; it just moves opaque tagged events:

```cpp
struct NetEvent {
    enum class Kind : uint8_t { Connected, Disconnected, Error, Message };
    Kind     kind;
    uint16_t opcode;     // valid when kind == Message
    uint8_t* payload;    // pooled buffer (NetBufferPool); null for non-Message
    uint32_t len;        // payload length
    // + a connection identifier (NetHandle) so the game knows which connection
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
- The engine owns the **lifecycle slot**: the io-object registry, pump scheduling, and teardown timing. The object's *code* must reside in a DLL alive as long as the engine holds it.
- `netlib` adapters: always loaded → engine holds them across reloads freely.
- `Game.dll` adapters: need a **pre-reload teardown hook** — the networking analog of today's `SystemScheduler::Clear()`.

### Where the teardown hook slots (verified)
`GameLibrary::LoadOrReload` (`src/engine/src/threading/GameLibrary.cpp:87-95`) on reload runs, in order: `GameExit(state)` → `m_Scheduler->Clear()` (destroys `Game.dll` `ISystem`s while vtables mapped) → `FreeLibrary` → swap → `GameRegisterSystems`. The Net teardown must run in that same pre-`FreeLibrary` window, and must do **more** than destroy objects: it must **quiesce the IO thread off any `Game.dll`-resident adapter** before unload (the IO thread runs adapter code; it cannot be mid-`Poll()` in a `Game.dll` adapter when `FreeLibrary` hits). Two viable placements (chosen in the Phase-2 spec):
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

    // Drain inbound (returns false when empty). Payload buffer is borrowed from
    // the NetBufferPool and valid until the next PollEvent on this handle.
    bool      (*PollEvent)(NetHandle, NetEvent* out);

    // Enqueue an outbound message (copies into the pool). False on backpressure.
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
3. **Framing lives in the adapter, on the IO thread.** Never in the engine; never in GameThread policy.
4. **The game never spawns threads.** It requests N io-servers / M client-connections; the engine owns all IO threads.
5. **Adapter residence decides reload survival** (Model B). `Game.dll`-resident adapters are torn down + IO-thread-quiesced before every `FreeLibrary`.
6. **`NetServices` is append-only.** Same ABI discipline as `NavServices`.
7. **Variable-length payloads never go inline in a ring.** Pooled buffer + `{ptr,len}` in the ring (the `StagingBufferPool` pattern).

## 11. Phasing

Each phase produces working, independently-testable software and has its own **context spec** (this doc is the umbrella). Implementation plans are written per phase via `writing-plans`.

| Phase | Deliverable | Proof of done | Context spec |
|-------|-------------|---------------|--------------|
| **1. netlib standalone** | Interfaces (`IIoClient`/`IIoServer`/stream/framing) + TCP impl (with length-prefix framing) + in-memory adapter. Zero engine dependency. | Unit tests: framing reassembly (partial reads, coalesced reads), in-memory loopback round-trip, TCP loopback round-trip. | `2026-05-30-netlib-standalone-design.md` |
| **2. Engine integration** | IO thread pool, io-object registry + pump, inbound/outbound rings + `NetBufferPool`, `NetServices` bridge (+ `SystemContext.Net`), Model-B pre-reload teardown + IO quiesce. | **Loopback round-trip**: a game `NetSystem` sends a message client↔server in one process via the in-memory adapter and observes it on GameThread; survives a `Game.dll` hot-reload when the adapter is `netlib`-resident. | `2026-05-30-net-engine-integration-design.md` |
| **3. Dedicated server (out-of-process)** | Headless boot path + `server.exe` target + editor spawn/supervise control + loopback-TCP client↔server wiring. | Editor launches `server.exe`; the in-editor client connects over loopback TCP and exchanges a message round-trip. | `2026-05-30-dedicated-server-out-of-process-design.md` |
| **4. In-process server (additive)** | `ServerHost` instantiated in-editor behind an in-memory adapter — **after** de-singletoning `NavMeshSystem` et al. | Deferred. Documented as a stub; not planned until Phase 3 lands and the singleton work is scoped. | (later) |

**Phase 1 is the dependency root** — everything else builds on its abstractions. Phases 3–4 get their plans when reached; Phase 4 additionally depends on a separate de-singletoning effort.

## 12. Open questions deferred to per-phase specs (not architecture-level)

- IO-thread → io-object assignment policy (one thread per server vs. shared pool with work-stealing) — perf decision, Phase 2.
- Backpressure policy on ring-full / pool-exhaustion (drop oldest, block-with-warning, grow) — Phase 2.
- Exact `ClientConfig` / `ServerConfig` shape (target address, listen port, buffer sizes, max connections) — Phases 1–2.
- `NetHandle` representation (index+salt like mesh handles vs. opaque pointer) — Phase 2.
- `server.exe` ↔ editor supervision protocol (process spawn, health, shutdown signal) — Phase 3.
```
