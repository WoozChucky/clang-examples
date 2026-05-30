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
- `TcpClient` / `TcpServer` concrete impls (Winsock2), framing on top.
- An `InMemory` adapter pair (two endpoints backed by in-process queues, no OS socket) — the loopback transport Phase 4 will reuse.
- A non-blocking **`Poll()`** model on each adapter: the *caller* (Phase 2's engine IO thread) drives `Poll()`; the adapter never owns a thread. (Engine owns all threads — architecture invariant #4.)
- Unit tests (`tests/test_netlib.cpp` or similar, following the `test_navmesh.cpp` style).

**Out of scope (later phases):**
- IO threads (Phase 2 owns threading; Phase 1 adapters are `Poll()`-driven, thread-agnostic).
- The `NetServices` bridge, rings, `NetBufferPool`, ECS, hot-reload (Phase 2).
- `server.exe` / dedicated server (Phase 3).
- TLS/encryption, UDP, reconnection logic, protocol semantics (game's job, or much later).

## Key design constraints & decisions

### Adapters are `Poll()`-driven, never thread-owning
The architecture forbids the game (and, by extension, the transport it picks) from spawning threads — the engine owns IO threads (invariant #4). So `netlib` adapters expose a **non-blocking pump**: the owner calls `Poll()` repeatedly from whatever thread it likes. `Poll()` does a non-blocking recv/send pass and surfaces results. In Phase 1 tests the test harness calls `Poll()` in a loop; in Phase 2 an engine IO thread does. This keeps `netlib` free of any threading policy.

Sketch (illustrative, finalized in the plan):
```cpp
namespace netlib {

// Discrete framed message + connection lifecycle, surfaced by Poll().
struct IoEvent {
    enum class Kind : uint8_t { Connected, Disconnected, Error, Message };
    Kind            kind;
    ConnId          conn;        // which connection (servers fan out to many)
    std::span<const std::byte> payload;  // valid until next Poll(); borrowed
};

class IIoClient {
public:
    virtual ~IIoClient();
    virtual bool Connect(const Endpoint& target) = 0;     // non-blocking initiate
    virtual void Send(std::span<const std::byte> frame) = 0; // framed by the impl
    virtual void Poll(std::vector<IoEvent>& out) = 0;     // non-blocking pump
    virtual void Close() = 0;
};

class IIoServer {
public:
    virtual ~IIoServer();
    virtual bool Listen(const Endpoint& bind) = 0;
    virtual void Send(ConnId conn, std::span<const std::byte> frame) = 0;
    virtual void Poll(std::vector<IoEvent>& out) = 0;     // accept + recv across all conns
    virtual void Close(ConnId conn) = 0;
    virtual void Shutdown() = 0;
};

} // namespace netlib
```
> Note: `IoEvent::payload` is netlib's **internal** event shape. Phase 2 translates it into the engine's `NetEvent` (which uses the `NetBufferPool`). They are deliberately separate types: netlib has no `NetBufferPool` dependency.

### Framing is per-adapter, length-prefixed (TCP)
- Wire format: `[uint32 lengthLE][payload bytes]`. The framer accumulates bytes across `Poll()` calls until a full frame is available, then emits it. Document endianness explicitly (little-endian on-wire chosen for this engine's x64-only target — see CLAUDE.md "Windows-only").
- The in-memory adapter need not length-prefix internally (it can pass whole blobs), but it MUST present the **same `IoEvent` discrete-message semantics** so the two are interchangeable behind the interface.
- A sane max-frame-size guard (reject/disconnect on absurd lengths) to avoid a malicious/garbage length prefix allocating gigabytes. Pick a configurable cap; default documented in the plan.

### Buffer ownership inside netlib
- `Poll()` surfaces `payload` as a **borrowed** span valid only until the next `Poll()` (the adapter owns the receive buffer). The Phase-2 engine layer copies into the `NetBufferPool` immediately on drain. This keeps netlib allocation-simple and matches how the engine will hand pooled buffers to the game.
- `Send()` takes a `span` the impl copies into its own send buffer before returning (caller's bytes need not outlive the call).

### Platform / build
- Winsock2 (`ws2_32.lib`), `WSAStartup`/`WSACleanup` lifecycle owned by netlib (ref-counted or once-init). Windows-only is fine (CLAUDE.md).
- `add_library(netlib SHARED ...)` mirroring `src/ecs/CMakeLists.txt`: `NETLIB_EXPORTS` define, `NETLIB_API` macro (`__declspec(dllexport/import)`) in a `netlib_api.h`, PUBLIC include dir, output to `RUNTIME_DIR`, `FOLDER Libraries`, MSVC `Embedded` debug info in Debug. New target → add to root `CMakeLists.txt` `add_subdirectory` list and any preset target lists.
- C++23, `NOMINMAX`, `WIN32_LEAN_AND_MEAN` (match global conventions). No GLM needed unless an `Endpoint` wants it (it doesn't — host string + port).
- A new unit-test target `test_netlib` mirroring `test_ecs` / `test_navmesh` wiring in CMake.

### Logging
- netlib must not depend on the engine's `SM_*` logging if that lives in `Engine.dll`. Check where `SM_TRACE`/`SM_WARN` are defined (`src/common/include/lib.h`, header-only macros → likely safe to use from any target linking `CommonHeaders`). If they are header-only and dependency-free, netlib may link `CommonHeaders` and use them; otherwise netlib gets a minimal injectable log callback. **Resolve this in the plan** — prefer reusing `SM_*` per the user's "log on degradation, never silent skip" rule (memory: `feedback_logging_over_silent_skip`).

## Testing strategy (TDD)

Follow `test_navmesh.cpp` conventions (plain assertion-style harness, `All <suite> tests passed.` on success). Concrete cases:
- **Framer unit tests** (no sockets): feed bytes in 1-byte chunks → expect N discrete frames; feed two frames in one buffer → expect both; feed a partial header then the rest → one frame; oversize length → error/disconnect, no huge alloc.
- **In-memory round-trip:** client `Send` → server `Poll` sees `Connected` then `Message` with identical bytes; reverse direction; `Close` surfaces `Disconnected`.
- **TCP loopback round-trip:** bind ephemeral port on `127.0.0.1`, connect, exchange, close. Drive both ends by interleaving `Poll()` calls in the test loop (no background threads in the test).
- **Multi-connection server:** two clients connect to one `IIoServer`; messages are attributed to the correct `ConnId`.

## Risks / gotchas

- **Winsock non-blocking correctness:** `WSAEWOULDBLOCK` is normal, not an error; partial sends must be retried (buffer the unsent tail). The framer/send-buffer must handle short writes.
- **Ephemeral port in tests:** bind to port 0, read back the assigned port, to avoid flaky fixed-port collisions in CI/local.
- **No background threads in Phase 1 tests** — determinism. The `Poll()` model makes this natural.
- **Don't leak the abstraction:** keep Winsock types out of the public headers (`IIoClient`/`IIoServer` must not expose `SOCKET`), so `Engine.dll` (which includes these) never sees TCP. PIMPL or a factory returning the interface.

## Deliverables checklist

- [ ] `netlib` SHARED target + `NETLIB_API` macro + CMake wiring (root + preset lists).
- [ ] Public headers: interfaces + `IoEvent` + `Endpoint` + factories (`MakeTcpClient`, `MakeTcpServer`, `MakeInMemoryPair`).
- [ ] Length-prefix framer (internal).
- [ ] TCP client/server impls (Winsock2, non-blocking, `Poll()`-driven).
- [ ] In-memory adapter pair.
- [ ] `test_netlib` target + the test cases above, all passing.
- [ ] No link to Engine/ecs/nvrhi/glfw (verify the link graph).
