# Phase 1 — netlib Standalone — Context Spec

**Umbrella:** `2026-05-30-networking-architecture-design.md`
**Date:** 2026-05-30
**Depends on:** nothing (this is the dependency root).
**Blocks:** Phase 2 (engine integration) and everything after.

---

## Goal

A standalone `netlib.dll` providing transport **abstractions** plus two concrete adapters (TCP, in-memory), with framing built into each adapter. **Zero dependency on `Engine.dll`, `ecs.dll`, the renderer, GLFW, or the ECS.** It must compile and unit-test in complete isolation — that isolation is the whole point: it proves the engine can depend on interfaces only, and lets a game supply its own adapter later.

## Why this phase exists / what success looks like

By the end of Phase 1 we can, in a unit test with no engine present:
1. Open an in-memory client↔server pair and round-trip a framed message.
2. Open a TCP client↔server over loopback and round-trip a framed message.
3. Feed the TCP framer a byte stream split at arbitrary boundaries (one byte at a time; two messages coalesced in one recv) and get back exactly the original discrete messages.

If those pass, the abstraction boundary is proven and Phase 2 can build on it.

## Scope

**In scope:**
- Public abstract interfaces: `IIoClient`, `IIoServer`, and the byte-stream/framing contract.
- A **length-prefix framer** (read `uint32` length, then exactly that many bytes; emit complete frames; buffer partials across calls).
- `TcpServer` = **Windows IOCP** (completion port + worker pool, overlapped `WSARecv`/`WSASend`), framing on the completing worker.
- `TcpClient` = **async non-blocking** (overlapped I/O / event-driven), **no busy-spin**. IOCP-single-worker is acceptable; not mandated.
- An `InMemory` adapter pair (two endpoints backed by in-process queues, no OS socket) — the loopback transport Phase 4 will reuse.
- A **push/sink** contract: each adapter owns its threading and pushes events into a caller-provided `IIoSink`. `Stop()` joins those threads.
- Unit tests (`tests/test_netlib.cpp` or similar, following the `test_navmesh.cpp` style).

**Out of scope (later phases):**
- The engine sink impl, lock-free MPSC ring, `NetBufferPool` (Phase 2 — netlib only *calls* an abstract `IIoSink`).
- The `NetServices` bridge, rings, `NetBufferPool`, ECS, hot-reload (Phase 2).
- `server.exe` / dedicated server (Phase 3).
- TLS/encryption, UDP, reconnection logic, protocol semantics (game's job, or much later).

## Key design constraints & decisions

### Why push/sink, not `Poll()` (§0 rationale)
A `Poll()` pull contract (caller's thread drives non-blocking recv) **cannot host IOCP**. IOCP associates sockets with a completion port and runs worker threads that **block on `GetQueuedCompletionStatus`**, waking only when the kernel completes an overlapped op — it must own its threads. Forcing IOCP behind a `Poll()` either throws away the kernel-completion model or hides IOCP behind an internal queue the caller polls (re-adding latency + a thread hop — exactly what IOCP exists to avoid). For a real-time MP server we want IOCP's scale + low latency, so the contract is **push**: the adapter owns threading and pushes into a sink.

### The push/sink contract — adapter owns threading
Each adapter owns whatever threads it needs (IOCP workers for the server; one async I/O thread for the client) and pushes events into a caller-provided `IIoSink`. The sink is called **from adapter threads** — for the IOCP server, from *many* worker threads concurrently — so it **must be thread-safe**. `Stop()` joins all adapter threads (the quiesce point for hot-reload, Phase 2).

Sketch (illustrative, finalized in the plan):
```cpp
namespace netlib {

// Discrete framed message + connection lifecycle.
struct IoEvent {
    enum class Kind : uint8_t { Connected, Disconnected, Error, Message };
    Kind            kind;
    ConnId          conn;        // which connection (servers fan out to many)
    std::span<const std::byte> payload;  // borrowed for the OnIoEvent call only
};

// Implemented by the CALLER (Phase-1 tests; Phase-2 engine). Called from
// adapter-owned thread(s) — MUST be thread-safe. Copy `payload` before returning.
class IIoSink { public: virtual ~IIoSink(); virtual void OnIoEvent(const IoEvent&) = 0; };

class IIoClient {
public:
    virtual ~IIoClient();
    virtual bool Start(const Endpoint& target, IIoSink* sink) = 0; // connect + spawn async I/O
    virtual void Send(std::span<const std::byte> frame) = 0;       // thread-safe; framed by impl
    virtual void Stop() = 0;                                       // joins threads
};

class IIoServer {
public:
    virtual ~IIoServer();
    virtual bool Start(const Endpoint& bind, IIoSink* sink) = 0;   // listen + spawn IOCP workers
    virtual void Send(ConnId conn, std::span<const std::byte> frame) = 0; // thread-safe
    virtual void Close(ConnId conn) = 0;
    virtual void Stop() = 0;                                       // joins all workers
};

} // namespace netlib
```
> `IoEvent::payload` is netlib's **internal** borrowed shape. Phase 2's engine sink copies it into the `NetBufferPool` *inside* `OnIoEvent`. netlib has no `NetBufferPool` dependency — the types stay separate.

### Framing is per-adapter, length-prefixed (TCP)
- Wire format: `[uint32 lengthLE][payload bytes]`. The framer accumulates bytes across recv completions until a full frame is available, then emits one `OnIoEvent(Message)`. Document endianness explicitly (little-endian on-wire; x64-only target — CLAUDE.md "Windows-only"). Per-connection framer state (the partial-frame buffer) — for the IOCP server, keep it per-connection and ensure a single outstanding `WSARecv` per connection so completions for one connection are serialized (no concurrent reframe of the same stream).
- The in-memory adapter need not length-prefix internally (it can pass whole blobs), but MUST present the **same discrete-message `IoEvent` semantics** so the two are interchangeable behind the interface.
- A sane max-frame-size guard (reject/disconnect on absurd lengths) to avoid a garbage/malicious length prefix allocating gigabytes. Configurable cap; default documented in the plan.

### Buffer ownership inside netlib
- `OnIoEvent` surfaces `payload` as a **borrowed** span valid only for the duration of the call (the adapter owns the receive buffer). The Phase-2 engine sink copies into the `NetBufferPool` immediately. Keeps netlib allocation-simple.
- `Send()` takes a `span` the impl copies into its own send buffer before returning (caller's bytes need not outlive the call). Per-connection sends serialize internally (queue + single outstanding `WSASend`) so frames don't interleave on the wire.

### Perf knobs (real-time MP)
- `TCP_NODELAY` **on by default** (disable Nagle — its ~40ms small-send batching is fatal for real-time), configurable per connection.
- `SO_REUSEADDR` on the listen socket (quick restart without `WSAEADDRINUSE`).
- Configurable `SO_SNDBUF`/`SO_RCVBUF`; sensible defaults documented in the plan.
- Prefer one outstanding overlapped recv per connection with a reused buffer (minimize per-recv allocation); scatter/gather `WSABUF` and zero-copy framing are a later optimization, not Phase 1.

### Platform / build
- Winsock2 (`ws2_32.lib`), `WSAStartup`/`WSACleanup` lifecycle owned by netlib (ref-counted or once-init). Windows-only is fine (CLAUDE.md).
- `add_library(netlib SHARED ...)` mirroring `src/ecs/CMakeLists.txt`: `NETLIB_EXPORTS` define, `NETLIB_API` macro (`__declspec(dllexport/import)`) in a `netlib_api.h`, PUBLIC include dir, output to `RUNTIME_DIR`, `FOLDER Libraries`, MSVC `Embedded` debug info in Debug. New target → add to root `CMakeLists.txt` `add_subdirectory` list and any preset target lists.
- C++23, `NOMINMAX`, `WIN32_LEAN_AND_MEAN` (match global conventions). No GLM needed unless an `Endpoint` wants it (it doesn't — host string + port).
- A new unit-test target `test_netlib` mirroring `test_ecs` / `test_navmesh` wiring in CMake.

### Logging
- netlib must not depend on the engine's `SM_*` logging if that lives in `Engine.dll`. Check where `SM_TRACE`/`SM_WARN` are defined (`src/common/include/lib.h`, header-only macros → likely safe to use from any target linking `CommonHeaders`). If they are header-only and dependency-free, netlib may link `CommonHeaders` and use them; otherwise netlib gets a minimal injectable log callback. **Resolve this in the plan** — prefer reusing `SM_*` per the user's "log on degradation, never silent skip" rule (memory: `feedback_logging_over_silent_skip`).

## Testing strategy (TDD)

Follow `test_navmesh.cpp` conventions (plain assertion-style harness, `All <suite> tests passed.` on success). The adapters now own threads, so socket tests implement a **test `IIoSink`** that records events into a thread-safe buffer + signals a condition variable; the test waits on it with a **timeout** (fail rather than hang). The framer is tested standalone (no threads/sockets).
- **Framer unit tests** (pure, no sockets, no threads): feed bytes in 1-byte chunks → expect N discrete frames; two frames in one buffer → expect both; partial header then the rest → one frame; oversize length → error, no huge alloc.
- **In-memory round-trip:** client `Send` → test sink on the server side observes `Connected` then `Message` with identical bytes; reverse direction; `Stop`/`Close` surfaces `Disconnected`. (In-memory delivers synchronously → deterministic, no waiting.)
- **TCP loopback round-trip:** bind ephemeral port on `127.0.0.1`; `Start` server + client with test sinks; exchange; assert via the sink (wait-with-timeout); `Stop` both. Real IOCP/async threads run here — synchronize through the sink, never `sleep()`-and-hope.
- **Multi-connection server:** two clients connect to one `IIoServer`; the server sink attributes messages to the correct `ConnId`. Exercises concurrent `OnIoEvent` from multiple IOCP workers → asserts the sink's thread-safety.
- **`Stop()` joins cleanly:** after `Stop()`, no further `OnIoEvent` fires and all worker threads have joined (the hot-reload quiesce contract Phase 2 relies on).

## Risks / gotchas

- **IOCP correctness is the bulk of the risk.** Overlapped lifetimes (each `OVERLAPPED` + its buffer must outlive the in-flight op), one outstanding `WSARecv` per connection, completion-key → connection mapping, graceful vs. abortive close, `0`-byte recv completion = peer closed. This is the hard part of Phase 1 — budget for it.
- **Sink thread-safety:** `OnIoEvent` runs concurrently across IOCP workers. The contract demands the *caller's* sink be thread-safe; netlib must document it loudly. The test sink must itself be correct (lock or lock-free) or tests flake.
- **`WSAEWOULDBLOCK` on the client / partial sends:** buffer the unsent tail; retry on writability. Per-connection send serialization avoids interleaved frames.
- **Ephemeral port in tests:** bind to port 0, read back the assigned port — avoid flaky fixed-port collisions.
- **`Stop()` ordering:** must stop accepting, cancel outstanding ops (`CancelIoEx`), post completion-port exit packets to wake workers, then join. Getting this wrong hangs the test (hence the timeouts) or crashes on reload later.
- **Don't leak the abstraction:** keep Winsock/IOCP types out of public headers (`IIoClient`/`IIoServer`/`IIoSink` must not expose `SOCKET`/`HANDLE`/`OVERLAPPED`), so `Engine.dll` (which includes these) never sees TCP. PIMPL or factory-returns-interface.

## Deliverables checklist

- [ ] `netlib` SHARED target + `NETLIB_API` macro + CMake wiring (root + preset lists).
- [ ] Public headers: `IIoClient`/`IIoServer`/`IIoSink` + `IoEvent` + `Endpoint` + connection config (`noDelay`, buffer sizes) + factories (`MakeTcpClient`, `MakeTcpServer`, `MakeInMemoryPair`).
- [ ] Length-prefix framer (internal, unit-tested standalone).
- [ ] `TcpServer` = IOCP (completion port + worker pool, overlapped recv/send, per-conn framing).
- [ ] `TcpClient` = async non-blocking (overlapped/event-driven, no busy-spin).
- [ ] In-memory adapter pair (synchronous sink delivery).
- [ ] Perf knobs wired: `TCP_NODELAY` default-on, `SO_REUSEADDR`, configurable buffers.
- [ ] `test_netlib` target + the cases above (sink-based, timeout-guarded), all passing.
- [ ] No link to Engine/ecs/nvrhi/glfw (verify the link graph).
