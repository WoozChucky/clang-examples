# netlib Standalone (Phase 1) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build `netlib.dll` — a standalone networking library with abstract transport interfaces, a length-prefix framer, an in-memory adapter, and a Windows-IOCP TCP server + async TCP client — with zero dependency on the engine/ECS/renderer.

**Architecture:** Ports-and-adapters. Public abstract interfaces (`IIoClient`/`IIoServer`/`IIoSink`) live in `include/netlib/`; concrete TCP (IOCP) + in-memory impls live in `src/` behind factories that return `unique_ptr<interface>` (Winsock/IOCP types never leak into public headers). Adapters own their threads and **push** events into a caller-provided thread-safe `IIoSink`; `Stop()` joins those threads.

**Tech Stack:** C++23, Winsock2 + IOCP (`ws2_32`), CMake SHARED lib mirroring `ecs.dll`, header-only `SM_*` logging from `src/common/include/lib.h`.

**Spec:** `docs/superpowers/specs/2026-05-30-netlib-standalone-design.md` (+ umbrella `2026-05-30-networking-architecture-design.md`).

---

## Conventions for every task

- **Build (configure once, then build the target):**
  ```
  cmake --preset msvc-win64-vs2026-community
  cmake --build --preset msvc-win64-vs2026-community --target test_netlib
  ```
  (Adding/removing a CMake file auto-triggers reconfigure on the next build. If a fresh subdir isn't picked up, re-run the `--preset` configure line.)
- **Run the test exe:**
  ```
  ./out/build/msvc-win64-vs2026-community/bin/Debug/test_netlib.exe
  ```
  Success prints `All netlib tests passed.` and returns 0.
- **Test harness style** (matches `tests/test_navmesh.cpp`): a `CHECK(cond, msg)` macro that logs + sets a failure flag; `main` runs sections, prints the summary line, returns non-zero on any failure.
- **Commit identity:** `Nuno Silva <nuno.levezinho@live.com.pt>`. Never `--no-verify`.
- **Branch:** work continues on `feat/networking-design` (already checked out).
- **Logging:** use `SM_TRACE/SM_WARN/SM_ERROR` (header-only). **Do not use `SM_ASSERT`** in netlib (it needs `platform_debug_break` at link). Every Winsock failure logs `WSAGetLastError()`. `WSAEWOULDBLOCK` is normal flow — never logged as error.

---

## File Structure

```
src/netlib/
  CMakeLists.txt                       SHARED lib target (mirrors src/ecs/CMakeLists.txt)
  include/netlib/
    netlib_api.h                       NETLIB_API dllexport/import macro
    Endpoint.h                         Endpoint{host,port}, ConnId, ConnConfig{noDelay,...}
    IoEvent.h                          IoEvent{Kind,conn,payload span}
    IIo.h                              IIoSink, IIoClient, IIoServer (pure virtual)
    netlib.h                           umbrella include + factories + Version()
  src/
    WinsockGuard.h / WinsockGuard.cpp  WSAStartup/WSACleanup ref-count + Version()
    Framer.h / Framer.cpp              internal length-prefix reassembly (unit-tested)
    InMemoryAdapter.cpp                MakeInMemoryPair() — synchronous sink delivery
    IocpCore.h / IocpCore.cpp          shared IOCP: per-conn state, worker loop, recv/send/frame/sink, Stop/join
    TcpServer.cpp                      MakeTcpServer() — accept thread + IocpCore
    TcpClient.cpp                      MakeTcpClient() — blocking connect + IocpCore (1 worker)
tests/
  test_netlib.cpp                      the suite (incl. a local platform_debug_break)
```

Public headers expose **no** Winsock/IOCP types. Impl classes are defined entirely in `.cpp` and handed back through the interface pointers.

---

## Task 1: netlib target skeleton + test harness

**Files:**
- Create: `src/netlib/include/netlib/netlib_api.h`
- Create: `src/netlib/include/netlib/netlib.h`
- Create: `src/netlib/src/WinsockGuard.h`
- Create: `src/netlib/src/WinsockGuard.cpp`
- Create: `src/netlib/CMakeLists.txt`
- Modify: `CMakeLists.txt` (add `add_subdirectory(src/netlib)`)
- Create: `tests/test_netlib.cpp`
- Modify: `tests/CMakeLists.txt` (add `test_netlib` target)

- [ ] **Step 1: Write the API macro header**

`src/netlib/include/netlib/netlib_api.h`:
```cpp
#pragma once

// dllexport when building netlib.dll, dllimport when consuming it. Mirrors ECS_API/ENGINE_API.
#ifndef NETLIB_API
  #ifdef _WIN32
    #ifdef NETLIB_EXPORTS
      #define NETLIB_API __declspec(dllexport)
    #else
      #define NETLIB_API __declspec(dllimport)
    #endif
  #else
    #define NETLIB_API
  #endif
#endif
```

- [ ] **Step 2: Write the Winsock ref-count guard + Version**

`src/netlib/src/WinsockGuard.h`:
```cpp
#pragma once

namespace netlib::detail {

// Ref-counted WSAStartup/WSACleanup. Each adapter holds one WinsockGuard for its
// lifetime; the first guard calls WSAStartup, the last destroyed calls WSACleanup.
class WinsockGuard {
public:
    WinsockGuard();
    ~WinsockGuard();
    WinsockGuard(const WinsockGuard&) = delete;
    WinsockGuard& operator=(const WinsockGuard&) = delete;
    bool Ok() const { return m_Ok; }
private:
    bool m_Ok = false;
};

} // namespace netlib::detail
```

`src/netlib/src/WinsockGuard.cpp`:
```cpp
#include "WinsockGuard.h"

#include <atomic>
#include <mutex>
#include <winsock2.h>

#include "lib.h"   // SM_WARN / SM_ERROR (header-only)

namespace netlib::detail {

namespace {
std::mutex g_Mx;
int        g_RefCount = 0;
}

WinsockGuard::WinsockGuard() {
    std::scoped_lock lk(g_Mx);
    if (g_RefCount == 0) {
        WSADATA wsa{};
        const int rc = WSAStartup(MAKEWORD(2, 2), &wsa);
        if (rc != 0) {
            SM_ERROR("netlib: WSAStartup failed (rc=%d)", rc);
            return;
        }
    }
    ++g_RefCount;
    m_Ok = true;
}

WinsockGuard::~WinsockGuard() {
    if (!m_Ok) return;
    std::scoped_lock lk(g_Mx);
    if (--g_RefCount == 0) {
        WSACleanup();
    }
}

} // namespace netlib::detail
```

- [ ] **Step 3: Write the umbrella public header with Version()**

`src/netlib/include/netlib/netlib.h`:
```cpp
#pragma once

#include "netlib/netlib_api.h"

namespace netlib {

// Returns a static version string. Smoke-test hook to prove the DLL links + loads.
NETLIB_API const char* Version();

} // namespace netlib
```

Add `Version()` impl at the bottom of `WinsockGuard.cpp` (one TU is fine for now):
```cpp
namespace netlib {
const char* Version() { return "netlib 0.1"; }
}
```

- [ ] **Step 4: Write the CMake target**

`src/netlib/CMakeLists.txt`:
```cmake
add_library(netlib SHARED
    src/WinsockGuard.cpp
)

target_include_directories(netlib
    PUBLIC  include
    PRIVATE src ../common/include
)

target_link_libraries(netlib PRIVATE
    ws2_32
)

target_compile_definitions(netlib PRIVATE
    NETLIB_EXPORTS
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

set_target_properties(netlib PROPERTIES
    OUTPUT_NAME netlib
    DEBUG_POSTFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    ARCHIVE_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    FOLDER Libraries
)

if(MSVC)
    set_property(TARGET netlib PROPERTY MSVC_DEBUG_INFORMATION_FORMAT
                 $<$<CONFIG:Debug>:Embedded>)
endif()
```

Add to root `CMakeLists.txt` immediately after the `add_subdirectory(src/common)` line (line 23):
```cmake
# netlib networking library (standalone; no engine dependency)
add_subdirectory(src/netlib)
```

- [ ] **Step 5: Write the test harness with one smoke test**

`tests/test_netlib.cpp`:
```cpp
#include <cstdio>
#include <cstring>

#include "netlib/netlib.h"

// Per-module SM_ASSERT backend (declared in lib.h, each exe provides its own).
// netlib itself avoids SM_ASSERT, but lib.h's templates may reference this; define
// it here so the test exe always links. Mirrors src/runtime/src/main.cpp.
void platform_debug_break(const char*, const char*, int, const char*) {}

static int g_Failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { std::printf("FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); ++g_Failures; } } while (0)

static void test_version() {
    const char* v = netlib::Version();
    CHECK(v != nullptr, "Version() returns non-null");
    CHECK(std::strncmp(v, "netlib", 6) == 0, "Version() starts with 'netlib'");
}

int main() {
    test_version();

    if (g_Failures == 0) { std::printf("All netlib tests passed.\n"); return 0; }
    std::printf("%d netlib test(s) FAILED.\n", g_Failures);
    return 1;
}
```

Add to `tests/CMakeLists.txt` (append at end):
```cmake
add_executable(test_netlib
    test_netlib.cpp
)

target_link_libraries(test_netlib PRIVATE
    CommonHeaders
    netlib
)

target_include_directories(test_netlib PRIVATE
    ${CMAKE_SOURCE_DIR}/src/netlib/include
    ${CMAKE_SOURCE_DIR}/src/netlib/src
)

set_target_properties(test_netlib PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    FOLDER Tests
)
```

- [ ] **Step 6: Configure, build, run**

Run:
```
cmake --preset msvc-win64-vs2026-community
cmake --build --preset msvc-win64-vs2026-community --target test_netlib
./out/build/msvc-win64-vs2026-community/bin/Debug/test_netlib.exe
```
Expected: build succeeds; output `All netlib tests passed.`

- [ ] **Step 7: Commit**

```bash
git add src/netlib tests/test_netlib.cpp tests/CMakeLists.txt CMakeLists.txt
git commit -m "feat(net): netlib SHARED target skeleton + test harness"
```

---

## Task 2: Public interfaces (Endpoint, IoEvent, IIoSink/Client/Server)

**Files:**
- Create: `src/netlib/include/netlib/Endpoint.h`
- Create: `src/netlib/include/netlib/IoEvent.h`
- Create: `src/netlib/include/netlib/IIo.h`
- Modify: `src/netlib/include/netlib/netlib.h` (factory declarations)
- Modify: `tests/test_netlib.cpp` (compile + value test)

- [ ] **Step 1: Write the failing test**

Add to `tests/test_netlib.cpp` (and call from `main` before the summary):
```cpp
#include "netlib/Endpoint.h"
#include "netlib/IoEvent.h"
#include "netlib/IIo.h"

static void test_value_types() {
    netlib::Endpoint ep{ "127.0.0.1", 7777 };
    CHECK(ep.port == 7777, "Endpoint.port set");
    CHECK(ep.host == "127.0.0.1", "Endpoint.host set");

    netlib::ConnConfig cfg{};
    CHECK(cfg.noDelay == true, "ConnConfig.noDelay defaults true (TCP_NODELAY on)");

    netlib::IoEvent ev{};
    ev.kind = netlib::IoEvent::Kind::Message;
    CHECK(ev.kind == netlib::IoEvent::Kind::Message, "IoEvent.kind assignable");
}
```
Add `test_value_types();` to `main`.

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build --preset msvc-win64-vs2026-community --target test_netlib`
Expected: FAIL — compile error, `netlib/Endpoint.h` not found.

- [ ] **Step 3: Write the headers**

`src/netlib/include/netlib/Endpoint.h`:
```cpp
#pragma once

#include <cstdint>
#include <string>

namespace netlib {

// A connection id, unique within one IIoServer (servers fan out to many peers).
// 0 is reserved as "invalid / not a specific connection" (used for client-side
// events where there is only one connection).
enum class ConnId : uint64_t { Invalid = 0 };

struct Endpoint {
    std::string host;   // e.g. "127.0.0.1" or a hostname
    uint16_t    port = 0;
};

// Per-connection tunables. Defaults chosen for real-time game traffic.
struct ConnConfig {
    bool     noDelay      = true;       // TCP_NODELAY on — disable Nagle (~40ms batching is fatal)
    int      sendBufBytes = 1 << 18;    // SO_SNDBUF (256 KiB); 0 = leave OS default
    int      recvBufBytes = 1 << 18;    // SO_RCVBUF
    uint32_t maxFrameBytes = 1u << 24;  // 16 MiB hard cap; oversize length prefix => disconnect
};

} // namespace netlib
```

`src/netlib/include/netlib/IoEvent.h`:
```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "netlib/Endpoint.h"

namespace netlib {

// One framed message OR a connection-lifecycle notification, pushed by the adapter
// into IIoSink::OnIoEvent from an adapter-owned thread.
struct IoEvent {
    enum class Kind : uint8_t { Connected, Disconnected, Error, Message };

    Kind   kind = Kind::Error;
    ConnId conn = ConnId::Invalid;
    // Valid only for kind == Message, and only for the duration of the OnIoEvent
    // call (borrowed — the adapter owns the buffer). Copy it before returning.
    std::span<const std::byte> payload;
};

} // namespace netlib
```

`src/netlib/include/netlib/IIo.h`:
```cpp
#pragma once

#include <cstddef>
#include <span>

#include "netlib/Endpoint.h"
#include "netlib/IoEvent.h"

namespace netlib {

// Implemented by the CALLER. Called from adapter-owned thread(s) — for the IOCP
// server, concurrently from many worker threads. MUST be thread-safe. Copy
// IoEvent::payload before returning (it is borrowed).
class IIoSink {
public:
    virtual ~IIoSink() = default;
    virtual void OnIoEvent(const IoEvent& ev) = 0;
};

// Outbound client connection. Owns its async I/O thread(s); Stop() joins them.
class IIoClient {
public:
    virtual ~IIoClient() = default;
    // Connect to `target` and begin async I/O, pushing events to `sink`.
    // Returns false if the connect could not be initiated. `sink` must outlive Stop().
    virtual bool Start(const Endpoint& target, const ConnConfig& cfg, IIoSink* sink) = 0;
    // Enqueue one framed message (thread-safe). Copies `payload` before returning.
    virtual void Send(std::span<const std::byte> payload) = 0;
    // Stop async I/O and join all threads. Idempotent. No OnIoEvent fires after it returns.
    virtual void Stop() = 0;
};

// Listening server. Owns its accept thread + IOCP worker pool; Stop() joins them.
class IIoServer {
public:
    virtual ~IIoServer() = default;
    virtual bool Start(const Endpoint& bind, const ConnConfig& cfg, IIoSink* sink) = 0;
    // Send to one connection (thread-safe). Copies `payload`.
    virtual void Send(ConnId conn, std::span<const std::byte> payload) = 0;
    virtual void Close(ConnId conn) = 0;     // drop one connection
    virtual void Stop() = 0;                 // stop accepting + join all workers
};

} // namespace netlib
```

- [ ] **Step 4: Declare factories in `netlib.h`**

Append to `src/netlib/include/netlib/netlib.h` (inside `namespace netlib`, before the closing brace):
```cpp
#include <memory>
#include "netlib/IIo.h"

// ... (Version declaration stays above)

// Concrete adapter factories. The returned object owns its own threading.
NETLIB_API std::unique_ptr<IIoServer> MakeTcpServer();
NETLIB_API std::unique_ptr<IIoClient> MakeTcpClient();

// In-memory loopback pair: events delivered synchronously on the caller's thread.
// `outServer`/`outClient` are linked; the client's Send reaches the server sink and
// vice versa. No socket, no OS permission, deterministic — for tests + in-process use.
struct InMemoryPair {
    std::unique_ptr<IIoServer> server;
    std::unique_ptr<IIoClient> client;
};
NETLIB_API InMemoryPair MakeInMemoryPair();
```
> Put the `#include`s at the top of the file, not inside the namespace — shown inline here only for locality. Move them above `namespace netlib {`.

- [ ] **Step 5: Run test to verify it passes**

The factories are declared but not defined; the value-type test only constructs structs, so it links **only if** `main` doesn't call a factory yet (it doesn't). Run:
```
cmake --build --preset msvc-win64-vs2026-community --target test_netlib
./out/build/msvc-win64-vs2026-community/bin/Debug/test_netlib.exe
```
Expected: PASS — `All netlib tests passed.`

- [ ] **Step 6: Commit**

```bash
git add src/netlib/include tests/test_netlib.cpp
git commit -m "feat(net): public interfaces — Endpoint, IoEvent, IIoSink/Client/Server"
```

---

## Task 3: Length-prefix framer (pure, unit-tested)

The framer turns a byte stream into discrete `[uint32 lengthLE][payload]` frames, buffering partial reads across calls. Pure logic, no sockets/threads.

**Files:**
- Create: `src/netlib/src/Framer.h`
- Create: `src/netlib/src/Framer.cpp`
- Modify: `src/netlib/CMakeLists.txt` (add `src/Framer.cpp`)
- Modify: `tests/test_netlib.cpp` (framer tests)

- [ ] **Step 1: Write the failing tests**

Add to `tests/test_netlib.cpp`:
```cpp
#include <vector>
#include "Framer.h"   // internal; test include dir covers src/netlib/src

static std::vector<std::byte> bytes_of(const char* s) {
    std::vector<std::byte> v;
    for (const char* p = s; *p; ++p) v.push_back(static_cast<std::byte>(*p));
    return v;
}

// Build a wire frame: [uint32 LE length][payload].
static std::vector<std::byte> wire_frame(const char* payload) {
    auto p = bytes_of(payload);
    std::vector<std::byte> out;
    const uint32_t len = static_cast<uint32_t>(p.size());
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::byte>((len >> (8 * i)) & 0xff));
    out.insert(out.end(), p.begin(), p.end());
    return out;
}

static void test_framer() {
    using netlib::detail::Framer;

    // (a) One frame fed one byte at a time -> exactly one emitted frame.
    {
        Framer f(1u << 24);
        auto wire = wire_frame("hello");
        std::vector<std::vector<std::byte>> got;
        bool ok = true;
        for (std::byte b : wire) {
            ok = ok && f.Push(std::span(&b, 1), [&](std::span<const std::byte> frame) {
                got.emplace_back(frame.begin(), frame.end());
            });
        }
        CHECK(ok, "framer: drip feed no error");
        CHECK(got.size() == 1, "framer: drip feed -> 1 frame");
        CHECK(got.size() == 1 && got[0] == bytes_of("hello"), "framer: drip payload correct");
    }

    // (b) Two frames coalesced in one buffer -> two emitted frames.
    {
        Framer f(1u << 24);
        auto w1 = wire_frame("aa");
        auto w2 = wire_frame("bbbb");
        std::vector<std::byte> both = w1;
        both.insert(both.end(), w2.begin(), w2.end());
        std::vector<std::vector<std::byte>> got;
        bool ok = f.Push(both, [&](std::span<const std::byte> fr) { got.emplace_back(fr.begin(), fr.end()); });
        CHECK(ok, "framer: coalesced no error");
        CHECK(got.size() == 2, "framer: coalesced -> 2 frames");
        CHECK(got.size() == 2 && got[0] == bytes_of("aa") && got[1] == bytes_of("bbbb"),
              "framer: coalesced payloads correct");
    }

    // (c) Oversize length prefix -> Push returns false (caller disconnects), no huge alloc.
    {
        Framer f(8);  // max 8-byte frames
        std::vector<std::byte> hdr;
        const uint32_t huge = 1000000;
        for (int i = 0; i < 4; ++i) hdr.push_back(static_cast<std::byte>((huge >> (8 * i)) & 0xff));
        bool ok = f.Push(hdr, [](std::span<const std::byte>) {});
        CHECK(!ok, "framer: oversize length -> Push returns false");
    }
}
```
Add `test_framer();` to `main`.

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build --preset msvc-win64-vs2026-community --target test_netlib`
Expected: FAIL — `Framer.h` not found.

- [ ] **Step 3: Implement the framer**

`src/netlib/src/Framer.h`:
```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace netlib::detail {

// Reassembles a [uint32 LE length][payload] byte stream into discrete frames.
// Stateful across Push() calls (buffers partial frames). Single-threaded: one
// Framer per connection, touched only by that connection's I/O thread.
class Framer {
public:
    explicit Framer(uint32_t maxFrameBytes) : m_Max(maxFrameBytes) {}

    using OnFrame = std::function<void(std::span<const std::byte>)>;

    // Feed received bytes. Calls `onFrame` once per complete frame. Returns false
    // if a length prefix exceeds maxFrameBytes (caller should disconnect); true otherwise.
    bool Push(std::span<const std::byte> in, const OnFrame& onFrame);

private:
    std::vector<std::byte> m_Buf;   // accumulated, not-yet-complete bytes
    uint32_t               m_Max;
};

} // namespace netlib::detail
```

`src/netlib/src/Framer.cpp`:
```cpp
#include "Framer.h"

#include <cstring>

#include "lib.h"  // SM_WARN

namespace netlib::detail {

bool Framer::Push(std::span<const std::byte> in, const OnFrame& onFrame) {
    m_Buf.insert(m_Buf.end(), in.begin(), in.end());

    size_t offset = 0;
    for (;;) {
        if (m_Buf.size() - offset < 4) break;  // not enough for a length prefix

        uint32_t len = 0;
        std::memcpy(&len, m_Buf.data() + offset, 4);  // x64 is little-endian; wire is LE

        if (len > m_Max) {
            SM_WARN("netlib: frame length %u exceeds max %u; disconnecting", len, m_Max);
            return false;
        }
        if (m_Buf.size() - offset - 4 < len) break;   // full payload not yet arrived

        const std::byte* payload = m_Buf.data() + offset + 4;
        onFrame(std::span<const std::byte>(payload, len));
        offset += 4 + len;
    }

    // Drop consumed bytes, keep the partial tail.
    if (offset > 0) m_Buf.erase(m_Buf.begin(), m_Buf.begin() + offset);
    return true;
}

} // namespace netlib::detail
```

Add `src/Framer.cpp` to the `netlib` sources in `src/netlib/CMakeLists.txt`:
```cmake
add_library(netlib SHARED
    src/WinsockGuard.cpp
    src/Framer.cpp
)
```

- [ ] **Step 4: Run to verify it passes**

Run + execute the exe. Expected: PASS — `All netlib tests passed.`

- [ ] **Step 5: Commit**

```bash
git add src/netlib/src/Framer.h src/netlib/src/Framer.cpp src/netlib/CMakeLists.txt tests/test_netlib.cpp
git commit -m "feat(net): length-prefix framer with partial/coalesced/oversize handling"
```

---

## Task 4: In-memory adapter pair (synchronous sink delivery)

A non-socket loopback: the client's `Send` synchronously frames and delivers to the server's sink (and vice versa), on the calling thread. Deterministic — no threads, no OS permission. Proves the interface end-to-end before IOCP.

**Files:**
- Create: `src/netlib/src/InMemoryAdapter.cpp`
- Modify: `src/netlib/CMakeLists.txt` (add source)
- Modify: `tests/test_netlib.cpp` (round-trip + lifecycle tests)

- [ ] **Step 1: Write the failing tests**

Add to `tests/test_netlib.cpp`:
```cpp
#include "netlib/netlib.h"
#include <mutex>

// Thread-safe recording sink (reused for socket tests later).
struct RecordingSink : netlib::IIoSink {
    std::mutex mx;
    std::vector<netlib::IoEvent::Kind> kinds;
    std::vector<std::vector<std::byte>> messages;
    int connected = 0, disconnected = 0;

    void OnIoEvent(const netlib::IoEvent& ev) override {
        std::scoped_lock lk(mx);
        kinds.push_back(ev.kind);
        if (ev.kind == netlib::IoEvent::Kind::Connected)    ++connected;
        if (ev.kind == netlib::IoEvent::Kind::Disconnected) ++disconnected;
        if (ev.kind == netlib::IoEvent::Kind::Message)
            messages.emplace_back(ev.payload.begin(), ev.payload.end());
    }
    size_t msgCount() { std::scoped_lock lk(mx); return messages.size(); }
};

static void test_inmemory() {
    auto pair = netlib::MakeInMemoryPair();
    RecordingSink serverSink, clientSink;

    netlib::ConnConfig cfg{};
    CHECK(pair.server->Start(netlib::Endpoint{}, cfg, &serverSink), "inmem: server Start");
    CHECK(pair.client->Start(netlib::Endpoint{}, cfg, &clientSink), "inmem: client Start");
    CHECK(serverSink.connected == 1, "inmem: server saw Connected");
    CHECK(clientSink.connected == 1, "inmem: client saw Connected");

    auto msg = bytes_of("ping");
    pair.client->Send(msg);
    CHECK(serverSink.msgCount() == 1, "inmem: server received 1 msg");
    CHECK(serverSink.messages[0] == bytes_of("ping"), "inmem: server payload correct");

    auto reply = bytes_of("pong");
    // server replies to the single client connection (ConnId 1 by convention).
    pair.server->Send(netlib::ConnId{1}, reply);
    CHECK(clientSink.msgCount() == 1, "inmem: client received reply");
    CHECK(clientSink.messages[0] == bytes_of("pong"), "inmem: client reply correct");

    pair.client->Stop();
    CHECK(serverSink.disconnected == 1, "inmem: server saw Disconnected after client Stop");
}
```
Add `test_inmemory();` to `main`.

- [ ] **Step 2: Run to verify it fails**

Run build. Expected: FAIL — link error, `MakeInMemoryPair` unresolved.

- [ ] **Step 3: Implement the in-memory adapter**

`src/netlib/src/InMemoryAdapter.cpp`:
```cpp
#include "netlib/netlib.h"

#include <memory>

namespace netlib {
namespace {

// Shared channel between the linked in-memory client + server. Synchronous:
// a Send immediately invokes the peer's sink on the caller's thread.
struct InMemChannel {
    IIoSink* serverSink = nullptr;   // server's sink (receives client->server)
    IIoSink* clientSink = nullptr;   // client's sink (receives server->client)
    bool     serverUp   = false;
    bool     clientUp   = false;
    static constexpr ConnId kClientConn = ConnId{1};
};

void Emit(IIoSink* sink, IoEvent::Kind kind, ConnId conn,
          std::span<const std::byte> payload = {}) {
    if (!sink) return;
    IoEvent ev{};
    ev.kind = kind;
    ev.conn = conn;
    ev.payload = payload;
    sink->OnIoEvent(ev);
}

class InMemServer final : public IIoServer {
public:
    explicit InMemServer(std::shared_ptr<InMemChannel> ch) : m_Ch(std::move(ch)) {}
    bool Start(const Endpoint&, const ConnConfig&, IIoSink* sink) override {
        m_Ch->serverSink = sink;
        m_Ch->serverUp = true;
        // If the client is already up, surface its connection now.
        if (m_Ch->clientUp) Emit(m_Ch->serverSink, IoEvent::Kind::Connected, InMemChannel::kClientConn);
        return true;
    }
    void Send(ConnId, std::span<const std::byte> payload) override {
        if (m_Ch->clientUp) Emit(m_Ch->clientSink, IoEvent::Kind::Message, ConnId::Invalid, payload);
    }
    void Close(ConnId) override { Stop(); }
    void Stop() override {
        if (!m_Ch->serverUp) return;
        m_Ch->serverUp = false;
        if (m_Ch->clientUp) Emit(m_Ch->clientSink, IoEvent::Kind::Disconnected, ConnId::Invalid);
    }
private:
    std::shared_ptr<InMemChannel> m_Ch;
};

class InMemClient final : public IIoClient {
public:
    explicit InMemClient(std::shared_ptr<InMemChannel> ch) : m_Ch(std::move(ch)) {}
    bool Start(const Endpoint&, const ConnConfig&, IIoSink* sink) override {
        m_Ch->clientSink = sink;
        m_Ch->clientUp = true;
        Emit(m_Ch->clientSink, IoEvent::Kind::Connected, ConnId::Invalid);
        if (m_Ch->serverUp) Emit(m_Ch->serverSink, IoEvent::Kind::Connected, InMemChannel::kClientConn);
        return true;
    }
    void Send(std::span<const std::byte> payload) override {
        if (m_Ch->serverUp)
            Emit(m_Ch->serverSink, IoEvent::Kind::Message, InMemChannel::kClientConn, payload);
    }
    void Stop() override {
        if (!m_Ch->clientUp) return;
        m_Ch->clientUp = false;
        if (m_Ch->serverUp)
            Emit(m_Ch->serverSink, IoEvent::Kind::Disconnected, InMemChannel::kClientConn);
    }
private:
    std::shared_ptr<InMemChannel> m_Ch;
};

} // namespace

InMemoryPair MakeInMemoryPair() {
    auto ch = std::make_shared<InMemChannel>();
    InMemoryPair p;
    p.server = std::make_unique<InMemServer>(ch);
    p.client = std::make_unique<InMemClient>(ch);
    return p;
}

} // namespace netlib
```

Add `src/InMemoryAdapter.cpp` to `src/netlib/CMakeLists.txt` sources.

- [ ] **Step 4: Run to verify it passes**

Build + run. Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/netlib/src/InMemoryAdapter.cpp src/netlib/CMakeLists.txt tests/test_netlib.cpp
git commit -m "feat(net): in-memory adapter pair (synchronous sink loopback)"
```

---

## Task 5: IOCP TCP server

> **USER ACTION (manual, before first run of this task's test):** The first time `test_netlib.exe` calls `listen()`, Windows may show the **Defender Firewall "Allow this app to communicate" dialog** (loopback often skips it, but policy varies). **Accept it.** If a firewall rule needs admin, run the authorizing command yourself via `! <cmd>`. The test uses bounded timeouts, so an unaccepted prompt makes it *fail with a log line*, not hang forever.

The shared IOCP machinery lives in `IocpCore`; the server wraps it with an accept thread. Per-connection: one outstanding `WSARecv`, a `Framer`, and a serialized send queue.

**Files:**
- Create: `src/netlib/src/IocpCore.h`
- Create: `src/netlib/src/IocpCore.cpp`
- Create: `src/netlib/src/TcpServer.cpp`
- Modify: `src/netlib/include/netlib/IIo.h` (add `BoundPort()` to `IIoServer`)
- Modify: `src/netlib/src/InMemoryAdapter.cpp` (add `BoundPort()` override returning 0 — else `InMemServer` is abstract and the build breaks)
- Modify: `src/netlib/CMakeLists.txt`
- Modify: `tests/test_netlib.cpp` (server test driven by a raw blocking client socket)

- [ ] **Step 1: Write the failing test (raw-socket client drives the real server)**

Add to `tests/test_netlib.cpp`:
```cpp
#include <winsock2.h>
#include <ws2tcpip.h>
#include <chrono>
#include <thread>

// Wait until `pred()` or timeout. Returns pred() final value. Avoids sleep-and-hope.
template <typename Pred>
static bool wait_until(Pred pred, int timeoutMs = 3000) {
    using clock = std::chrono::steady_clock;
    auto start = clock::now();
    while (!pred()) {
        if (std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - start).count() > timeoutMs)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

// Connect a raw blocking socket to 127.0.0.1:port, return the socket or INVALID_SOCKET.
static SOCKET raw_connect(uint16_t port) {
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return s;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::closesocket(s); return INVALID_SOCKET;
    }
    return s;
}

static void send_framed(SOCKET s, const char* payload) {
    auto p = bytes_of(payload);
    uint32_t len = static_cast<uint32_t>(p.size());
    ::send(s, reinterpret_cast<const char*>(&len), 4, 0);   // x64 LE
    ::send(s, reinterpret_cast<const char*>(p.data()), static_cast<int>(p.size()), 0);
}

static void test_tcp_server() {
    // The raw-socket helpers need WSAStartup in THIS exe too (netlib has its own).
    WSADATA wsa{}; WSAStartup(MAKEWORD(2,2), &wsa);

    auto server = netlib::MakeTcpServer();
    RecordingSink sink;
    netlib::ConnConfig cfg{};
    // Port 0 => OS picks an ephemeral port; we read it back via the bind endpoint.
    netlib::Endpoint bind{ "127.0.0.1", 0 };
    CHECK(server->Start(bind, cfg, &sink), "tcpsrv: Start");

    uint16_t port = server->BoundPort();   // see IIoServer addition below
    CHECK(port != 0, "tcpsrv: bound to a real port");

    SOCKET c = raw_connect(port);
    CHECK(c != INVALID_SOCKET, "tcpsrv: raw client connected");
    CHECK(wait_until([&]{ return sink.connected == 1; }), "tcpsrv: server saw Connected");

    send_framed(c, "hello-server");
    CHECK(wait_until([&]{ return sink.msgCount() == 1; }), "tcpsrv: server received frame");
    CHECK(sink.messages.size() == 1 && sink.messages[0] == bytes_of("hello-server"),
          "tcpsrv: server payload correct");

    ::closesocket(c);
    CHECK(wait_until([&]{ return sink.disconnected == 1; }), "tcpsrv: server saw Disconnected");

    server->Stop();
    WSACleanup();
}
```
Add `test_tcp_server();` to `main`.

> Add `BoundPort()` to `IIoServer` in `src/netlib/include/netlib/IIo.h`:
> ```cpp
>     // Port actually bound (resolves an ephemeral port chosen for port 0). 0 if not started.
>     virtual uint16_t BoundPort() const = 0;
> ```
> (Append-only addition; in-memory server returns 0.) **You must also** add `uint16_t BoundPort() const override { return 0; }` to `InMemServer` in `src/netlib/src/InMemoryAdapter.cpp` — `BoundPort` is now pure virtual, so `InMemServer` is abstract until it overrides it, and `MakeInMemoryPair` won't compile otherwise.

- [ ] **Step 2: Run to verify it fails**

Build. Expected: FAIL — `MakeTcpServer` unresolved / `BoundPort` missing.

- [ ] **Step 3: Implement `IocpCore`**

`src/netlib/src/IocpCore.h`:
```cpp
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>
#include <span>

#include <winsock2.h>

#include "netlib/Endpoint.h"
#include "netlib/IoEvent.h"
#include "netlib/IIo.h"
#include "Framer.h"

namespace netlib::detail {

// A single overlapped operation. `ov` MUST be the first member (CONTAINING_RECORD).
struct IoOp {
    OVERLAPPED ov{};
    enum class Type : uint8_t { Recv, Send } type;
    std::vector<std::byte> buffer;   // owns bytes for this op (recv scratch / send copy)
    WSABUF wsabuf{};
};

// Per-connection state. Lifetime managed by IocpCore under m_Mx; an atomic op
// refcount keeps it alive until all outstanding overlapped ops complete.
struct Conn {
    SOCKET  sock = INVALID_SOCKET;
    ConnId  id   = ConnId::Invalid;
    Framer  framer;
    std::mutex            sendMx;
    std::deque<std::vector<std::byte>> sendQ;
    bool                  sending = false;
    std::atomic<int>      outstanding{0};   // # of overlapped ops in flight
    std::atomic<bool>     closing{false};

    explicit Conn(uint32_t maxFrame) : framer(maxFrame) {}
};

// Shared IOCP engine used by both TcpServer and TcpClient. Owns the completion
// port + worker pool. Connections are registered via Register(); the workers run
// recv->frame->sink and drain send queues. Stop() cancels + joins everything.
class IocpCore {
public:
    IocpCore() = default;
    ~IocpCore();

    bool Start(IIoSink* sink, const ConnConfig& cfg, int workerCount);
    // Take ownership of an accepted/connected socket; post the first recv. Returns its ConnId.
    ConnId Register(SOCKET sock);
    void Send(ConnId id, std::span<const std::byte> payload);
    void Close(ConnId id);
    void Stop();   // idempotent; cancels ops, posts exit packets, joins workers

private:
    void WorkerLoop();
    void PostRecv(Conn* c);
    void PostSend(Conn* c);                 // call with sendMx held; posts head of queue
    void HandleRecv(Conn* c, IoOp* op, DWORD bytes, bool ok);
    void HandleSend(Conn* c, IoOp* op, DWORD bytes, bool ok);
    void Emit(ConnId id, IoEvent::Kind kind, std::span<const std::byte> payload = {});
    void Disconnect(Conn* c);               // emit Disconnected once + close socket
    void ReleaseOp(Conn* c, IoOp* op);      // decrement outstanding; free conn if dead

    HANDLE       m_Iocp = nullptr;
    IIoSink*     m_Sink = nullptr;
    ConnConfig   m_Cfg{};
    std::vector<std::thread> m_Workers;
    std::atomic<bool> m_Running{false};

    std::mutex   m_Mx;                       // guards m_Conns + m_NextId
    std::unordered_map<uint64_t, std::unique_ptr<Conn>> m_Conns;
    uint64_t     m_NextId = 1;               // ConnId 0 reserved invalid
};

} // namespace netlib::detail
```

`src/netlib/src/IocpCore.cpp`:
```cpp
#include "IocpCore.h"

#include <mswsock.h>

#include "lib.h"   // SM_TRACE / SM_WARN / SM_ERROR

#pragma comment(lib, "ws2_32.lib")

namespace netlib::detail {

namespace { constexpr ULONG_PTR kExitKey = 0; }   // PostQueuedCompletionStatus exit sentinel

IocpCore::~IocpCore() { Stop(); }

bool IocpCore::Start(IIoSink* sink, const ConnConfig& cfg, int workerCount) {
    m_Sink = sink;
    m_Cfg  = cfg;
    m_Iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
    if (!m_Iocp) { SM_ERROR("netlib: CreateIoCompletionPort failed (%lu)", GetLastError()); return false; }
    m_Running.store(true, std::memory_order_release);
    if (workerCount < 1) workerCount = 1;
    for (int i = 0; i < workerCount; ++i) m_Workers.emplace_back([this]{ WorkerLoop(); });
    return true;
}

ConnId IocpCore::Register(SOCKET sock) {
    // Apply per-connection options (TCP_NODELAY + buffer sizes).
    if (m_Cfg.noDelay) {
        BOOL on = TRUE;
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof(on));
    }
    if (m_Cfg.sendBufBytes > 0)
        setsockopt(sock, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&m_Cfg.sendBufBytes), sizeof(int));
    if (m_Cfg.recvBufBytes > 0)
        setsockopt(sock, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&m_Cfg.recvBufBytes), sizeof(int));

    Conn* raw = nullptr;
    ConnId id{};
    {
        std::scoped_lock lk(m_Mx);
        const uint64_t n = m_NextId++;
        auto c = std::make_unique<Conn>(m_Cfg.maxFrameBytes);
        c->sock = sock;
        c->id   = ConnId{n};
        raw = c.get();
        m_Conns.emplace(n, std::move(c));
        id = ConnId{n};
    }
    // Associate the socket with the completion port; completion key = Conn*.
    if (!CreateIoCompletionPort(reinterpret_cast<HANDLE>(sock), m_Iocp,
                                reinterpret_cast<ULONG_PTR>(raw), 0)) {
        SM_ERROR("netlib: associate socket to IOCP failed (%lu)", GetLastError());
        Close(id);
        return ConnId::Invalid;
    }
    Emit(id, IoEvent::Kind::Connected);
    PostRecv(raw);
    return id;
}

void IocpCore::PostRecv(Conn* c) {
    if (c->closing.load(std::memory_order_acquire)) return;
    auto* op = new IoOp();
    op->type = IoOp::Type::Recv;
    op->buffer.resize(64 * 1024);
    op->wsabuf.buf = reinterpret_cast<CHAR*>(op->buffer.data());
    op->wsabuf.len = static_cast<ULONG>(op->buffer.size());
    DWORD flags = 0, bytes = 0;
    c->outstanding.fetch_add(1, std::memory_order_acq_rel);
    const int rc = WSARecv(c->sock, &op->wsabuf, 1, &bytes, &flags, &op->ov, nullptr);
    if (rc == SOCKET_ERROR) {
        const int err = WSAGetLastError();
        if (err != WSA_IO_PENDING) {
            if (err != WSAECONNRESET && err != WSAECONNABORTED)
                SM_WARN("netlib: WSARecv failed (%d)", err);
            ReleaseOp(c, op);     // undo refcount + free op
            Disconnect(c);
        }
    }
}

void IocpCore::Send(ConnId id, std::span<const std::byte> payload) {
    std::scoped_lock lk(m_Mx);
    auto it = m_Conns.find(static_cast<uint64_t>(id));
    if (it == m_Conns.end()) return;
    Conn* c = it->second.get();

    // Frame: [uint32 LE length][payload], copied into a send buffer.
    std::vector<std::byte> buf;
    const uint32_t len = static_cast<uint32_t>(payload.size());
    buf.resize(4 + payload.size());
    std::memcpy(buf.data(), &len, 4);
    if (!payload.empty()) std::memcpy(buf.data() + 4, payload.data(), payload.size());

    std::scoped_lock slk(c->sendMx);
    c->sendQ.push_back(std::move(buf));
    if (!c->sending) { c->sending = true; PostSend(c); }
}

void IocpCore::PostSend(Conn* c) {           // sendMx held
    if (c->sendQ.empty()) { c->sending = false; return; }
    auto* op = new IoOp();
    op->type = IoOp::Type::Send;
    op->buffer = std::move(c->sendQ.front());
    c->sendQ.pop_front();
    op->wsabuf.buf = reinterpret_cast<CHAR*>(op->buffer.data());
    op->wsabuf.len = static_cast<ULONG>(op->buffer.size());
    DWORD bytes = 0;
    c->outstanding.fetch_add(1, std::memory_order_acq_rel);
    const int rc = WSASend(c->sock, &op->wsabuf, 1, &bytes, 0, &op->ov, nullptr);
    if (rc == SOCKET_ERROR) {
        const int err = WSAGetLastError();
        if (err != WSA_IO_PENDING) {
            if (err != WSAECONNRESET && err != WSAECONNABORTED)
                SM_WARN("netlib: WSASend failed (%d)", err);
            c->sending = false;
            ReleaseOp(c, op);
            Disconnect(c);
        }
    }
}

void IocpCore::WorkerLoop() {
    for (;;) {
        DWORD bytes = 0; ULONG_PTR key = 0; OVERLAPPED* ov = nullptr;
        const BOOL ok = GetQueuedCompletionStatus(m_Iocp, &bytes, &key, &ov, INFINITE);
        if (key == kExitKey && ov == nullptr) return;   // exit packet
        if (!ov) continue;
        auto* op = CONTAINING_RECORD(ov, IoOp, ov);
        auto* c  = reinterpret_cast<Conn*>(key);
        if (op->type == IoOp::Type::Recv) HandleRecv(c, op, bytes, ok == TRUE);
        else                              HandleSend(c, op, bytes, ok == TRUE);
    }
}

void IocpCore::HandleRecv(Conn* c, IoOp* op, DWORD bytes, bool ok) {
    if (!ok || bytes == 0) {            // error or graceful peer close
        ReleaseOp(c, op);
        Disconnect(c);
        return;
    }
    const bool framerOk = c->framer.Push(
        std::span<const std::byte>(op->buffer.data(), bytes),
        [&](std::span<const std::byte> frame) { Emit(c->id, IoEvent::Kind::Message, frame); });
    ReleaseOp(c, op);
    if (!framerOk) { Disconnect(c); return; }      // oversize frame
    PostRecv(c);                                   // keep one recv outstanding
}

void IocpCore::HandleSend(Conn* c, IoOp* op, DWORD /*bytes*/, bool ok) {
    ReleaseOp(c, op);
    if (!ok) { Disconnect(c); return; }
    std::scoped_lock slk(c->sendMx);
    PostSend(c);                                   // send next queued frame, if any
}

void IocpCore::Emit(ConnId id, IoEvent::Kind kind, std::span<const std::byte> payload) {
    if (!m_Sink) return;
    IoEvent ev{}; ev.kind = kind; ev.conn = id; ev.payload = payload;
    m_Sink->OnIoEvent(ev);
}

void IocpCore::Disconnect(Conn* c) {
    bool expected = false;
    if (!c->closing.compare_exchange_strong(expected, true)) return;  // once
    if (c->sock != INVALID_SOCKET) { ::closesocket(c->sock); c->sock = INVALID_SOCKET; }
    Emit(c->id, IoEvent::Kind::Disconnected);
    // Free the Conn only when no ops are outstanding (last ReleaseOp frees it).
    if (c->outstanding.load(std::memory_order_acquire) == 0) {
        std::scoped_lock lk(m_Mx);
        m_Conns.erase(static_cast<uint64_t>(c->id));
    }
}

void IocpCore::ReleaseOp(Conn* c, IoOp* op) {
    delete op;
    const int remaining = c->outstanding.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (remaining == 0 && c->closing.load(std::memory_order_acquire)) {
        std::scoped_lock lk(m_Mx);
        m_Conns.erase(static_cast<uint64_t>(c->id));
    }
}

void IocpCore::Close(ConnId id) {
    std::scoped_lock lk(m_Mx);
    auto it = m_Conns.find(static_cast<uint64_t>(id));
    if (it != m_Conns.end()) {
        // closesocket forces outstanding ops to complete with error → Disconnect path.
        if (it->second->sock != INVALID_SOCKET) ::closesocket(it->second->sock);
    }
}

void IocpCore::Stop() {
    if (!m_Running.exchange(false)) return;
    // Close all sockets to force outstanding ops to complete.
    {
        std::scoped_lock lk(m_Mx);
        for (auto& [n, c] : m_Conns)
            if (c->sock != INVALID_SOCKET) { ::closesocket(c->sock); c->sock = INVALID_SOCKET; }
    }
    // Wake every worker with an exit packet, then join.
    for (size_t i = 0; i < m_Workers.size(); ++i)
        PostQueuedCompletionStatus(m_Iocp, 0, kExitKey, nullptr);
    for (auto& t : m_Workers) if (t.joinable()) t.join();
    m_Workers.clear();
    { std::scoped_lock lk(m_Mx); m_Conns.clear(); }
    if (m_Iocp) { CloseHandle(m_Iocp); m_Iocp = nullptr; }
}

} // namespace netlib::detail
```

> **Implementer note:** `IocpCore` is the highest-risk code in Phase 1. The outstanding-op refcount + `closing` flag govern `Conn` lifetime; verify no `Conn` is freed while an op is in flight (the spec's "lock-free correctness" caution applies). Test `Stop()` joins under load. A residual race on `m_Conns.erase` from two paths (Disconnect vs ReleaseOp) is guarded by `m_Mx` + the refcount==0 check, but re-derive it carefully during review.

`src/netlib/src/TcpServer.cpp`:
```cpp
#include "netlib/netlib.h"
#include "IocpCore.h"
#include "WinsockGuard.h"

#include <atomic>
#include <thread>
#include <ws2tcpip.h>

#include "lib.h"

namespace netlib {
namespace {

class TcpServer final : public IIoServer {
public:
    bool Start(const Endpoint& bind, const ConnConfig& cfg, IIoSink* sink) override {
        if (!m_Wsa.Ok()) return false;
        m_Listen = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (m_Listen == INVALID_SOCKET) { SM_ERROR("netlib: listen socket failed (%d)", WSAGetLastError()); return false; }
        BOOL reuse = TRUE;
        setsockopt(m_Listen, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(bind.port);
        inet_pton(AF_INET, bind.host.empty() ? "0.0.0.0" : bind.host.c_str(), &addr.sin_addr);
        if (::bind(m_Listen, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            SM_ERROR("netlib: bind %s:%u failed (%d)", bind.host.c_str(), bind.port, WSAGetLastError());
            ::closesocket(m_Listen); m_Listen = INVALID_SOCKET; return false;
        }
        // Read back the actually-bound port (resolves port 0).
        sockaddr_in got{}; int gotLen = sizeof(got);
        if (getsockname(m_Listen, reinterpret_cast<sockaddr*>(&got), &gotLen) == 0)
            m_Port = ntohs(got.sin_port);

        if (::listen(m_Listen, SOMAXCONN) != 0) {
            SM_ERROR("netlib: listen failed (%d)", WSAGetLastError());
            ::closesocket(m_Listen); m_Listen = INVALID_SOCKET; return false;
        }
        // Worker pool sized to hardware concurrency (clamped); fine to start small.
        int workers = static_cast<int>(std::thread::hardware_concurrency());
        if (workers < 1) workers = 1; if (workers > 8) workers = 8;
        if (!m_Core.Start(sink, cfg, workers)) { ::closesocket(m_Listen); m_Listen = INVALID_SOCKET; return false; }

        m_Accepting.store(true, std::memory_order_release);
        m_AcceptThread = std::thread([this]{ AcceptLoop(); });
        SM_TRACE("netlib: TcpServer listening on %s:%u", bind.host.c_str(), m_Port);
        return true;
    }

    void Send(ConnId conn, std::span<const std::byte> payload) override { m_Core.Send(conn, payload); }
    void Close(ConnId conn) override { m_Core.Close(conn); }
    uint16_t BoundPort() const override { return m_Port; }

    void Stop() override {
        if (!m_Accepting.exchange(false)) { m_Core.Stop(); return; }
        if (m_Listen != INVALID_SOCKET) { ::closesocket(m_Listen); m_Listen = INVALID_SOCKET; } // unblocks accept()
        if (m_AcceptThread.joinable()) m_AcceptThread.join();
        m_Core.Stop();
    }
    ~TcpServer() override { Stop(); }

private:
    void AcceptLoop() {
        while (m_Accepting.load(std::memory_order_acquire)) {
            SOCKET s = ::accept(m_Listen, nullptr, nullptr);
            if (s == INVALID_SOCKET) {
                if (!m_Accepting.load(std::memory_order_acquire)) break;  // closed by Stop()
                const int err = WSAGetLastError();
                if (err == WSAEINTR || err == WSAENOTSOCK) break;
                SM_WARN("netlib: accept failed (%d)", err);
                continue;
            }
            m_Core.Register(s);
        }
    }

    detail::WinsockGuard m_Wsa;
    detail::IocpCore     m_Core;
    SOCKET               m_Listen = INVALID_SOCKET;
    uint16_t             m_Port   = 0;
    std::atomic<bool>    m_Accepting{false};
    std::thread          m_AcceptThread;
};

} // namespace

std::unique_ptr<IIoServer> MakeTcpServer() { return std::make_unique<TcpServer>(); }

} // namespace netlib
```

Add `src/IocpCore.cpp` and `src/TcpServer.cpp` to `src/netlib/CMakeLists.txt` sources.

- [ ] **Step 4: Run to verify it passes**

Build + run (accept the firewall prompt if it appears). Expected: PASS — `All netlib tests passed.`

- [ ] **Step 5: Commit**

```bash
git add src/netlib/src/IocpCore.h src/netlib/src/IocpCore.cpp src/netlib/src/TcpServer.cpp \
        src/netlib/include/netlib/IIo.h src/netlib/src/InMemoryAdapter.cpp \
        src/netlib/CMakeLists.txt tests/test_netlib.cpp
git commit -m "feat(net): IOCP TCP server (accept thread + completion-port worker pool)"
```

---

## Task 6: Async TCP client + full round-trip + multi-connection

The client reuses `IocpCore` with a single worker: blocking `connect()` on a connect thread, then hand the socket to `IocpCore` (overlapped recv/send). Then test a real client↔server round-trip and a two-client server.

**Files:**
- Create: `src/netlib/src/TcpClient.cpp`
- Modify: `src/netlib/CMakeLists.txt`
- Modify: `tests/test_netlib.cpp` (round-trip + multi-conn tests)

- [ ] **Step 1: Write the failing tests**

Add to `tests/test_netlib.cpp`:
```cpp
static void test_tcp_roundtrip() {
    auto server = netlib::MakeTcpServer();
    RecordingSink srvSink;
    netlib::ConnConfig cfg{};
    CHECK(server->Start(netlib::Endpoint{ "127.0.0.1", 0 }, cfg, &srvSink), "rt: server Start");
    uint16_t port = server->BoundPort();

    auto client = netlib::MakeTcpClient();
    RecordingSink cliSink;
    CHECK(client->Start(netlib::Endpoint{ "127.0.0.1", port }, cfg, &cliSink), "rt: client Start");
    CHECK(wait_until([&]{ return cliSink.connected == 1; }), "rt: client Connected");
    CHECK(wait_until([&]{ return srvSink.connected == 1; }), "rt: server Connected");

    client->Send(bytes_of("from-client"));
    CHECK(wait_until([&]{ return srvSink.msgCount() == 1; }), "rt: server got client msg");
    CHECK(srvSink.messages[0] == bytes_of("from-client"), "rt: server payload");

    // Reply to that connection (its ConnId is the first registered = 1).
    server->Send(netlib::ConnId{1}, bytes_of("from-server"));
    CHECK(wait_until([&]{ return cliSink.msgCount() == 1; }), "rt: client got server reply");
    CHECK(cliSink.messages[0] == bytes_of("from-server"), "rt: client payload");

    client->Stop();
    server->Stop();
}

static void test_tcp_multiconn() {
    auto server = netlib::MakeTcpServer();
    RecordingSink srvSink;
    netlib::ConnConfig cfg{};
    server->Start(netlib::Endpoint{ "127.0.0.1", 0 }, cfg, &srvSink);
    uint16_t port = server->BoundPort();

    auto c1 = netlib::MakeTcpClient(); RecordingSink s1;
    auto c2 = netlib::MakeTcpClient(); RecordingSink s2;
    c1->Start(netlib::Endpoint{ "127.0.0.1", port }, cfg, &s1);
    c2->Start(netlib::Endpoint{ "127.0.0.1", port }, cfg, &s2);
    CHECK(wait_until([&]{ return srvSink.connected == 2; }), "multi: server saw 2 Connected");

    c1->Send(bytes_of("one"));
    c2->Send(bytes_of("two"));
    CHECK(wait_until([&]{ return srvSink.msgCount() == 2; }), "multi: server got 2 msgs");
    // Both payloads present (order across connections not guaranteed).
    {
        std::scoped_lock lk(srvSink.mx);
        bool sawOne = false, sawTwo = false;
        for (auto& m : srvSink.messages) { if (m == bytes_of("one")) sawOne = true; if (m == bytes_of("two")) sawTwo = true; }
        CHECK(sawOne && sawTwo, "multi: both payloads attributed");
    }

    c1->Stop(); c2->Stop(); server->Stop();
}
```
Add both to `main`.

- [ ] **Step 2: Run to verify it fails**

Build. Expected: FAIL — `MakeTcpClient` unresolved.

- [ ] **Step 3: Implement the client**

`src/netlib/src/TcpClient.cpp`:
```cpp
#include "netlib/netlib.h"
#include "IocpCore.h"
#include "WinsockGuard.h"

#include <atomic>
#include <thread>
#include <ws2tcpip.h>

#include "lib.h"

namespace netlib {
namespace {

class TcpClient final : public IIoClient {
public:
    bool Start(const Endpoint& target, const ConnConfig& cfg, IIoSink* sink) override {
        if (!m_Wsa.Ok()) return false;
        if (!m_Core.Start(sink, cfg, /*workerCount*/ 1)) return false;
        m_Connecting.store(true, std::memory_order_release);
        // Blocking connect on a dedicated thread so Start() returns immediately and
        // the caller is never blocked; the socket then lives in IocpCore (overlapped).
        m_ConnectThread = std::thread([this, target]{ ConnectLoop(target); });
        return true;
    }

    void Send(std::span<const std::byte> payload) override {
        const ConnId id = m_Conn.load(std::memory_order_acquire);
        if (id != ConnId::Invalid) m_Core.Send(id, payload);
    }

    void Stop() override {
        m_Connecting.store(false, std::memory_order_release);
        if (m_ConnectThread.joinable()) m_ConnectThread.join();
        m_Core.Stop();
    }
    ~TcpClient() override { Stop(); }

private:
    void ConnectLoop(const Endpoint& target) {
        SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) { SM_ERROR("netlib: client socket failed (%d)", WSAGetLastError()); return; }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(target.port);
        if (inet_pton(AF_INET, target.host.c_str(), &addr.sin_addr) != 1) {
            SM_ERROR("netlib: client bad host '%s'", target.host.c_str());
            ::closesocket(s); return;
        }
        if (!m_Connecting.load(std::memory_order_acquire)) { ::closesocket(s); return; }
        if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            SM_WARN("netlib: client connect %s:%u failed (%d)", target.host.c_str(), target.port, WSAGetLastError());
            ::closesocket(s); return;
        }
        const ConnId id = m_Core.Register(s);    // emits Connected + posts recv
        m_Conn.store(id, std::memory_order_release);
    }

    detail::WinsockGuard  m_Wsa;
    detail::IocpCore      m_Core;
    std::atomic<ConnId>   m_Conn{ ConnId::Invalid };
    std::atomic<bool>     m_Connecting{false};
    std::thread           m_ConnectThread;
};

} // namespace

std::unique_ptr<IIoClient> MakeTcpClient() { return std::make_unique<TcpClient>(); }

} // namespace netlib
```

Add `src/TcpClient.cpp` to `src/netlib/CMakeLists.txt` sources.

- [ ] **Step 4: Run to verify it passes**

Build + run. Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/netlib/src/TcpClient.cpp src/netlib/CMakeLists.txt tests/test_netlib.cpp
git commit -m "feat(net): async TCP client (blocking connect + IOCP recv/send) + round-trip tests"
```

---

## Task 7: Clean-shutdown + oversize-frame-over-wire + link-graph guard

Harden the contract: `Stop()` joins with no event after it; an oversize length prefix over the wire disconnects; verify the link graph (no Engine/ecs).

**Files:**
- Modify: `tests/test_netlib.cpp`

- [ ] **Step 1: Write the failing tests**

Add to `tests/test_netlib.cpp`:
```cpp
static void test_stop_is_clean() {
    auto server = netlib::MakeTcpServer();
    RecordingSink srvSink;
    netlib::ConnConfig cfg{};
    server->Start(netlib::Endpoint{ "127.0.0.1", 0 }, cfg, &srvSink);
    uint16_t port = server->BoundPort();

    auto client = netlib::MakeTcpClient(); RecordingSink cliSink;
    client->Start(netlib::Endpoint{ "127.0.0.1", port }, cfg, &cliSink);
    CHECK(wait_until([&]{ return cliSink.connected == 1; }), "stop: connected");

    client->Stop();          // must join its threads
    server->Stop();          // must join accept thread + workers
    const size_t kindsAfter = [&]{ std::scoped_lock lk(srvSink.mx); return srvSink.kinds.size(); }();
    // Give any (incorrectly) lingering thread a moment; count must not grow.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const size_t kindsLater = [&]{ std::scoped_lock lk(srvSink.mx); return srvSink.kinds.size(); }();
    CHECK(kindsAfter == kindsLater, "stop: no events fire after Stop() returns");
}

static void test_oversize_frame_disconnects() {
    auto server = netlib::MakeTcpServer();
    RecordingSink srvSink;
    netlib::ConnConfig cfg{};
    cfg.maxFrameBytes = 16;          // tiny cap
    server->Start(netlib::Endpoint{ "127.0.0.1", 0 }, cfg, &srvSink);
    uint16_t port = server->BoundPort();

    WSADATA wsa{}; WSAStartup(MAKEWORD(2,2), &wsa);
    SOCKET c = raw_connect(port);
    CHECK(c != INVALID_SOCKET, "oversize: raw connect");
    CHECK(wait_until([&]{ return srvSink.connected == 1; }), "oversize: server connected");

    uint32_t huge = 1000000;         // > maxFrameBytes
    ::send(c, reinterpret_cast<const char*>(&huge), 4, 0);
    CHECK(wait_until([&]{ return srvSink.disconnected == 1; }), "oversize: server disconnects on bad length");

    ::closesocket(c);
    server->Stop();
    WSACleanup();
}
```
Add both to `main`.

- [ ] **Step 2: Run to verify it fails**

If the implementation from Tasks 5–6 is correct, these may *pass* immediately (they assert existing contract). If they fail, fix the implementation (e.g. `Stop()` not joining, or framer error not propagated to `Disconnect`). Run and observe.

- [ ] **Step 3: Fix any gaps surfaced**

If `test_stop_is_clean` fails: ensure `TcpServer::Stop` joins the accept thread *and* `IocpCore::Stop` joins workers before returning. If `test_oversize_frame_disconnects` fails: ensure `HandleRecv` calls `Disconnect(c)` when `framer.Push` returns false (it does in the Task-5 code). Make minimal fixes.

- [ ] **Step 4: Run to verify it passes**

Build + run. Expected: PASS — `All netlib tests passed.`

- [ ] **Step 5: Verify the link graph (no engine deps)**

Run:
```
dumpbin /dependents ./out/build/msvc-win64-vs2026-community/bin/Debug/netlib.dll
```
Expected: lists `ws2_32.dll` / system DLLs (and the MSVC runtime) — **NOT** `Engine.dll`, `ecs.dll`, `nvrhi*`, or `glfw*`. If any appear, remove the offending include/link.

- [ ] **Step 6: Commit**

```bash
git add tests/test_netlib.cpp
git commit -m "test(net): clean-shutdown, oversize-frame disconnect, link-graph guard"
```

---

## Final verification

- [ ] Full clean build of the test target + run, all sections pass:
  ```
  cmake --build --preset msvc-win64-vs2026-community --target test_netlib
  ./out/build/msvc-win64-vs2026-community/bin/Debug/test_netlib.exe
  ```
  Expected: `All netlib tests passed.`
- [ ] Confirm `netlib.dll` links no engine/ecs/renderer (Task 7 Step 5).
- [ ] Per subagent-driven-development: a final code-review pass over `IocpCore` (lifetime/refcount correctness under concurrency) before the branch advances.

---

## Notes for the executor

- **`IocpCore` is the risk centre.** The `Conn` lifetime (outstanding-op refcount + `closing` + `m_Mx`) is the part to scrutinize in review. Run the socket tests several times (concurrency bugs are intermittent). If a flake appears, suspect a `Conn` freed with an op still in flight, or a missing `m_Mx` guard around `m_Conns`.
- **The client uses IOCP with one worker** (spec allows IOCP-single-worker for the client; reuses `IocpCore` for DRY). It is genuinely async — no busy-spin.
- **AcceptEx/ConnectEx are deliberately not used** — a blocking accept thread + blocking connect thread keep the code tractable and are ample for the ARPG (a server world doesn't need 10k accepts/sec yet, the client holds ~2 connections). Note this as a future optimization, not a gap.
- **Firewall/UAC** is a user-owned manual gate (Task 5 banner). Tests time out rather than hang if a prompt is unaccepted.
