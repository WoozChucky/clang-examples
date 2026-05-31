# Agnostic Zero-Copy Net Transport Reshape — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Reshape the engine + netlib networking transport into a serialization-agnostic framed-byte pipe that removes the avoidable send/recv copies (two send prepends, the recv framer-accumulate, the recv drain) and the per-recv 64 KB allocation, reusing `NetBufferPool` for both directions.

**Architecture:** netlib gains a move-only `OwnedBuffer` (reserved 4-byte length head + type-erased deleter) so a single buffer travels game→wire by move with the length written in place. Recv lands directly in the `Framer`'s persistent buffer. The engine drops the `[u16 opcode]` from its API (type-tagging becomes the game's payload concern), allocates send buffers from a new `NetBufferPool` send instance (≤16 KB; heap fallback >16 KB), and exposes the inbound pool block directly in `PollEvent` (no drain copy).

**Tech Stack:** C++23, Windows IOCP/Winsock, CMake, MSVC (msvc-win64-vs2026-enterprise preset). Lock-free `MpscRing`/`NetBufferPool`.

**Build/verify commands:**
- Configure: `cmake --preset msvc-win64-vs2026-enterprise`
- Build a target: `cmake --build out/build/msvc-win64-vs2026-enterprise --target <t>`
- netlib tests: build target `test_netlib`, run `./out/build/msvc-win64-vs2026-enterprise/bin/Debug/test_netlib.exe` → expect `All netlib tests passed.`
- engine net tests: build target `test_net`, run `./out/build/msvc-win64-vs2026-enterprise/bin/Debug/test_net.exe` → expect `All net tests passed.`

**Reference:** spec `docs/superpowers/specs/2026-05-31-agnostic-zerocopy-net-transport-design.md`.

**Staging note (build-green boundaries):** Tasks 1–3 are netlib-internal; Task 2 also updates `NetSubsystem::Send` to an interim heap-backed `OwnedBuffer` (still copies, still carries opcode) so **Engine/game/editor keep compiling and all tests stay green after every task**. Task 4 swaps the interim path for the send pool, drops the opcode from the engine API, expands recv to expose the pool block, and ports the demo systems + engine tests in one cohesive change (they share the API surface, so they migrate together).

**Field-name note:** `NetEvent::payload`/`len` are kept (they already mean "opaque bytes"); only `NetEvent::opcode` is removed. The spec's `frame` name is cosmetic — keeping `payload` minimizes churn.

---

### Task 1: `OwnedBuffer` move-only buffer type

**Files:**
- Create: `src/netlib/include/netlib/OwnedBuffer.h`
- Create: `src/netlib/src/OwnedBuffer.cpp`
- Modify: `src/netlib/CMakeLists.txt` (add the .cpp)
- Test: `tests/test_netlib.cpp` (new `test_owned_buffer`, registered in `main`)

- [ ] **Step 1: Write the header**

Create `src/netlib/include/netlib/OwnedBuffer.h`:
```cpp
#pragma once

#include <cstddef>
#include <cstdint>

#include "netlib/netlib_api.h"

namespace netlib {

// Move-only buffer with a reserved length-prefix head. The producer writes the
// payload into Payload()[0 .. PayloadLen); the transport writes the 4-byte LE
// length prefix into the head and sends [Data(), Data()+TotalSize()). On
// destruction (or move-assign overwrite) the deleter reclaims the backing memory
// — this is what lets the transport stay agnostic to where the bytes came from
// (a pool block, a heap allocation, a test buffer).
class NETLIB_API OwnedBuffer {
public:
    static constexpr uint32_t kHeadBytes = 4;
    // ctx is opaque to the buffer; del receives it plus the base pointer.
    using Deleter = void (*)(void* ctx, std::byte* base) noexcept;

    OwnedBuffer() noexcept = default;
    // base points at the head (kHeadBytes reserved); base..base+kHeadBytes+payloadLen must be valid.
    OwnedBuffer(std::byte* base, uint32_t payloadLen, void* ctx, Deleter del) noexcept
        : m_Base(base), m_PayloadLen(payloadLen), m_Ctx(ctx), m_Del(del) {}

    OwnedBuffer(OwnedBuffer&& o) noexcept { MoveFrom(o); }
    OwnedBuffer& operator=(OwnedBuffer&& o) noexcept {
        if (this != &o) { Reset(); MoveFrom(o); }
        return *this;
    }
    OwnedBuffer(const OwnedBuffer&) = delete;
    OwnedBuffer& operator=(const OwnedBuffer&) = delete;
    ~OwnedBuffer() { Reset(); }

    [[nodiscard]] bool       Valid()      const noexcept { return m_Base != nullptr; }
    [[nodiscard]] std::byte* Data()       const noexcept { return m_Base; }                  // head + payload
    [[nodiscard]] std::byte* Payload()    const noexcept { return m_Base + kHeadBytes; }     // writable payload
    [[nodiscard]] uint32_t   PayloadLen() const noexcept { return m_PayloadLen; }
    [[nodiscard]] uint32_t   TotalSize()  const noexcept { return kHeadBytes + m_PayloadLen; }

private:
    void Reset() noexcept {
        if (m_Base && m_Del) m_Del(m_Ctx, m_Base);
        m_Base = nullptr; m_Del = nullptr; m_Ctx = nullptr; m_PayloadLen = 0;
    }
    void MoveFrom(OwnedBuffer& o) noexcept {
        m_Base = o.m_Base; m_PayloadLen = o.m_PayloadLen; m_Ctx = o.m_Ctx; m_Del = o.m_Del;
        o.m_Base = nullptr; o.m_Del = nullptr; o.m_Ctx = nullptr; o.m_PayloadLen = 0;
    }

    std::byte* m_Base = nullptr;
    uint32_t   m_PayloadLen = 0;
    void*      m_Ctx = nullptr;
    Deleter    m_Del = nullptr;
};

// Heap-backed OwnedBuffer of (kHeadBytes + payloadLen) bytes; deleter = delete[].
// Used by tests and by the engine's >16 KB send fallback.
NETLIB_API OwnedBuffer MakeHeapBuffer(uint32_t payloadLen);

} // namespace netlib
```

- [ ] **Step 2: Write the .cpp**

Create `src/netlib/src/OwnedBuffer.cpp`:
```cpp
#include "netlib/OwnedBuffer.h"

namespace netlib {

OwnedBuffer MakeHeapBuffer(uint32_t payloadLen) {
    auto* base = new std::byte[OwnedBuffer::kHeadBytes + payloadLen];
    return OwnedBuffer(base, payloadLen, /*ctx*/ nullptr,
                       [](void*, std::byte* b) noexcept { delete[] b; });
}

} // namespace netlib
```

- [ ] **Step 3: Add the .cpp to the netlib target**

In `src/netlib/CMakeLists.txt`, add `src/OwnedBuffer.cpp` to the netlib library's source list (next to the other `src/*.cpp` entries). Read the file first to match the existing list style.

- [ ] **Step 4: Write the failing test**

In `tests/test_netlib.cpp`, add after `test_value_types()` and add the include `#include "netlib/OwnedBuffer.h"` near the other netlib includes:
```cpp
static void test_owned_buffer() {
    using netlib::OwnedBuffer;

    // (a) MakeHeapBuffer: payload writable, sizes correct.
    {
        OwnedBuffer b = netlib::MakeHeapBuffer(5);
        CHECK(b.Valid(), "ownedbuf: heap buffer valid");
        CHECK(b.PayloadLen() == 5, "ownedbuf: payload len 5");
        CHECK(b.TotalSize() == 9, "ownedbuf: total = head(4)+5");
        CHECK(b.Payload() == b.Data() + 4, "ownedbuf: payload past 4-byte head");
        for (int i = 0; i < 5; ++i) b.Payload()[i] = static_cast<std::byte>('a' + i);
        CHECK(b.Payload()[0] == static_cast<std::byte>('a'), "ownedbuf: payload writable");
    }

    // (b) Deleter runs exactly once on destruction; move transfers ownership.
    {
        static int freed = 0; freed = 0;
        std::byte storage[8] = {};
        auto del = [](void* ctx, std::byte*) noexcept { ++*static_cast<int*>(ctx); };
        {
            OwnedBuffer a(storage, /*payloadLen*/ 4, &freed, del);
            OwnedBuffer moved = std::move(a);
            CHECK(!a.Valid(), "ownedbuf: moved-from invalid");
            CHECK(moved.Valid() && moved.PayloadLen() == 4, "ownedbuf: moved-to owns");
            CHECK(freed == 0, "ownedbuf: deleter not run while alive");
        }
        CHECK(freed == 1, "ownedbuf: deleter ran exactly once after scope");
    }
}
```
Register it in `main()` right after `test_value_types();`:
```cpp
    test_owned_buffer();
```
Add `#include <utility>` near the top of `tests/test_netlib.cpp` (for `std::move`) if not already present.

- [ ] **Step 5: Build and run — expect FAIL→PASS**

Run:
```
cmake --preset msvc-win64-vs2026-enterprise
cmake --build out/build/msvc-win64-vs2026-enterprise --target test_netlib
./out/build/msvc-win64-vs2026-enterprise/bin/Debug/test_netlib.exe
```
Expected: builds; output ends `All netlib tests passed.` (the new `test_owned_buffer` checks pass).

- [ ] **Step 6: Commit**
```
git add src/netlib/include/netlib/OwnedBuffer.h src/netlib/src/OwnedBuffer.cpp src/netlib/CMakeLists.txt tests/test_netlib.cpp
git commit -m "feat(net): add move-only OwnedBuffer with reserved length head"
```

---

### Task 2: netlib `Send` takes `OwnedBuffer&&` (+ interim engine bridge)

**Files:**
- Modify: `src/netlib/include/netlib/IIo.h` (Send signatures + include)
- Modify: `src/netlib/src/IocpCore.h` (`Conn::sendQ`, `IoOp::buffer`, `Send` decl)
- Modify: `src/netlib/src/IocpCore.cpp` (`Send`, `PostSend`)
- Modify: `src/netlib/src/TcpClient.cpp` (`Send`)
- Modify: `src/netlib/src/TcpServer.cpp` (`Send`)
- Modify: `src/netlib/src/InMemoryAdapter.cpp` (`Send` on both adapters)
- Modify: `src/engine/src/network/NetSubsystem.cpp` (interim: build a heap `OwnedBuffer`)
- Test: `tests/test_netlib.cpp` (update `Send` call sites + `RecordingSink` echo)

- [ ] **Step 1: Change the `IIo` Send signatures**

In `src/netlib/include/netlib/IIo.h`: add `#include "netlib/OwnedBuffer.h"`, then change the two `Send` declarations:
```cpp
    // IIoClient
    virtual void Send(OwnedBuffer&& payload) = 0;   // was: std::span<const std::byte>
```
```cpp
    // IIoServer
    virtual void Send(ConnId conn, OwnedBuffer&& payload) = 0;   // was: ConnId, span
```
Update the doc comments to say "Takes ownership of a framed-payload buffer (length head reserved); writes the length prefix in place." `std::span` may still be included for `IIoSink`/`IoEvent`; leave that include.

- [ ] **Step 2: Update `IocpCore` storage + decl**

In `src/netlib/src/IocpCore.h`:
- `#include "netlib/OwnedBuffer.h"`.
- `IoOp`: replace `std::vector<std::byte> buffer;` with two distinct buffers so recv and send don't alias (recv still uses a vector scratch in THIS task; Task 3 removes it):
  ```cpp
      std::vector<std::byte> recvBuffer;   // recv scratch (removed in Task 3)
      OwnedBuffer            sendBuffer;    // owns the framed bytes for a send op
  ```
- `Conn`: `std::deque<std::vector<std::byte>> sendQ;` → `std::deque<OwnedBuffer> sendQ;`
- `Send` decl: `void Send(ConnId id, OwnedBuffer&& payload);`

- [ ] **Step 3: Update `IocpCore::Send` + `PostSend` + recv buffer references**

In `src/netlib/src/IocpCore.cpp`:

Replace `Send` (currently builds a `[len][payload]` copy) with an in-place length write + move:
```cpp
void IocpCore::Send(ConnId id, OwnedBuffer&& payload) {
    std::shared_ptr<Conn> c;
    {
        std::scoped_lock lk(m_Mx);
        auto it = m_Conns.find(static_cast<uint64_t>(id));
        if (it == m_Conns.end()) return;   // payload destructs here -> deleter reclaims
        c = it->second;
    }

    // Write the [uint32 LE length] prefix into the reserved head, in place. No copy.
    const uint32_t len = payload.PayloadLen();
    std::memcpy(payload.Data(), &len, 4);   // x64 LE; head is the first 4 bytes

    std::scoped_lock slk(c->sendMx);
    c->sendQ.push_back(std::move(payload));
    if (!c->sending) { c->sending = true; PostSend(c); }
}
```
Replace the body of `PostSend` that moved the vector with the OwnedBuffer move + wsabuf over the whole frame:
```cpp
void IocpCore::PostSend(const std::shared_ptr<Conn>& c) {   // sendMx held; m_Mx NOT held
    if (c->sendQ.empty()) { c->sending = false; return; }
    auto* op = new IoOp();
    op->type = IoOp::Type::Send;
    op->conn = c;
    op->sendBuffer = std::move(c->sendQ.front());
    c->sendQ.pop_front();
    op->wsabuf.buf = reinterpret_cast<CHAR*>(op->sendBuffer.Data());
    op->wsabuf.len = static_cast<ULONG>(op->sendBuffer.TotalSize());
    const SOCKET h = c->sock.load(std::memory_order_acquire);
    if (h == INVALID_SOCKET) { c->sending = false; delete op; return; }
    DWORD bytes = 0;
    const int rc = WSASend(h, &op->wsabuf, 1, &bytes, 0, &op->ov, nullptr);
    if (rc == SOCKET_ERROR) {
        const int err = WSAGetLastError();
        if (err != WSA_IO_PENDING) {
            if (err != WSAECONNRESET && err != WSAECONNABORTED)
                SM_WARN("netlib: WSASend failed (%d)", err);
            c->sending = false;
            delete op;            // drops Conn ref AND destructs sendBuffer -> deleter reclaims
            Disconnect(c);
        }
    }
}
```
In `PostRecv` and `HandleRecv`, rename the recv buffer references from `op->buffer` to `op->recvBuffer` (the recv path is otherwise unchanged in this task):
- `PostRecv`: `op->recvBuffer.resize(64 * 1024);` `op->wsabuf.buf = reinterpret_cast<CHAR*>(op->recvBuffer.data());` `op->wsabuf.len = static_cast<ULONG>(op->recvBuffer.size());`
- `HandleRecv`: `c->framer.Push(std::span<const std::byte>(op->recvBuffer.data(), bytes), ...)`.

(`#include <cstring>` is already present for `memcpy`.)

- [ ] **Step 4: Update TcpClient / TcpServer / InMemory Send**

`src/netlib/src/TcpClient.cpp`:
```cpp
    void Send(OwnedBuffer&& payload) override {
        const ConnId id = m_Conn.load(std::memory_order_acquire);
        if (id != ConnId::Invalid) m_Core.Send(id, std::move(payload));
        // if not connected, payload destructs here -> deleter reclaims (message dropped)
    }
```
`src/netlib/src/TcpServer.cpp` — read it first; its `Send(ConnId, span)` forwards to `m_Core.Send`. Change to:
```cpp
    void Send(ConnId conn, OwnedBuffer&& payload) override {
        m_Core.Send(conn, std::move(payload));
    }
```
(and add `#include "netlib/OwnedBuffer.h"` if not transitively present).

`src/netlib/src/InMemoryAdapter.cpp` — the in-memory adapters deliver synchronously to the peer sink as a borrowed span. Change both `Send`s to consume the `OwnedBuffer`, deliver its payload span, then let it destruct:
```cpp
    // InMemServer
    void Send(ConnId, OwnedBuffer&& payload) override {
        if (m_Ch->clientUp)
            Emit(m_Ch->clientSink, IoEvent::Kind::Message, ConnId::Invalid,
                 std::span<const std::byte>(payload.Payload(), payload.PayloadLen()));
        // payload destructs here (deleter reclaims) AFTER the synchronous sink call returns
    }
```
```cpp
    // InMemClient
    void Send(OwnedBuffer&& payload) override {
        if (m_Ch->serverUp)
            Emit(m_Ch->serverSink, IoEvent::Kind::Message, InMemChannel::kClientConn,
                 std::span<const std::byte>(payload.Payload(), payload.PayloadLen()));
    }
```
Add `#include "netlib/OwnedBuffer.h"` to `InMemoryAdapter.cpp`. (The sink is called synchronously before the buffer destructs, so the borrowed span is valid for the call — matching the existing borrow contract.)

- [ ] **Step 5: Interim — keep `NetSubsystem::Send` compiling (heap OwnedBuffer)**

In `src/engine/src/network/NetSubsystem.cpp`, `NetSubsystem::Send` currently builds a `std::vector<std::byte>` `[opcode][data]` and passes it as a span. Change ONLY the handoff to build a heap `OwnedBuffer` (interim — still prepends opcode, still copies; Task 4 replaces this whole method):
```cpp
bool NetSubsystem::Send(NetHandle h, NetConnId conn, uint16_t opcode, const uint8_t* data, size_t len) {
    std::scoped_lock lk(m_Mx);
    auto it = m_Adapters.find(static_cast<uint32_t>(h));
    if (it == m_Adapters.end()) return false;

    // Interim bridge to netlib's OwnedBuffer API. Builds [u16 opcode][data] into a
    // heap-backed buffer (still the pre-reshape copy + opcode; replaced in the send-pool task).
    const uint32_t payloadLen = kOpcodeBytes + static_cast<uint32_t>(len);
    netlib::OwnedBuffer buf = netlib::MakeHeapBuffer(payloadLen);
    std::byte* p = buf.Payload();
    p[0] = static_cast<std::byte>(opcode & 0xff);
    p[1] = static_cast<std::byte>((opcode >> 8) & 0xff);
    if (len) std::memcpy(p + kOpcodeBytes, data, len);

    if (it->second.server) { it->second.server->Send(netlib::ConnId{ conn }, std::move(buf)); return true; }
    if (it->second.client) { it->second.client->Send(std::move(buf)); return true; }
    return false;
}
```
Add `#include <netlib/OwnedBuffer.h>` to `NetSubsystem.cpp`. (NetSubsystem already links netlib.)

- [ ] **Step 6: Update test_netlib.cpp Send call sites + echo sink**

In `tests/test_netlib.cpp`, add a helper near `bytes_of`:
```cpp
// Build a heap OwnedBuffer whose payload is a copy of `s` (for tests that send bytes).
static netlib::OwnedBuffer owned_of(const char* s) {
    auto v = bytes_of(s);
    netlib::OwnedBuffer b = netlib::MakeHeapBuffer(static_cast<uint32_t>(v.size()));
    for (size_t i = 0; i < v.size(); ++i) b.Payload()[i] = v[i];
    return b;
}
static netlib::OwnedBuffer owned_of(std::span<const std::byte> s) {
    netlib::OwnedBuffer b = netlib::MakeHeapBuffer(static_cast<uint32_t>(s.size()));
    for (size_t i = 0; i < s.size(); ++i) b.Payload()[i] = s[i];
    return b;
}
```
Update `RecordingSink::OnIoEvent` echo line:
```cpp
        if (ev.kind == netlib::IoEvent::Kind::Message && echoServer)
            echoServer->Send(ev.conn, owned_of(ev.payload));   // echo via OwnedBuffer
```
Update the direct send call sites:
- `test_inmemory`: `pair.client->Send(owned_of("ping"));` and `pair.server->Send(netlib::ConnId{1}, owned_of("pong"));` (drop the now-unused `msg`/`reply` locals or keep for the CHECK comparisons — keep the `bytes_of("ping")` comparisons).
- `test_tcp_roundtrip`: `client->Send(owned_of("from-client"));` and `server->Send(netlib::ConnId{1}, owned_of("from-server"));`.
- `test_tcp_multiconn`: `c1->Send(owned_of("one"));` `c2->Send(owned_of("two"));`.

- [ ] **Step 7: Build + run netlib tests, AND confirm engine still builds**

Run:
```
cmake --build out/build/msvc-win64-vs2026-enterprise --target test_netlib
./out/build/msvc-win64-vs2026-enterprise/bin/Debug/test_netlib.exe
cmake --build out/build/msvc-win64-vs2026-enterprise --target test_net
./out/build/msvc-win64-vs2026-enterprise/bin/Debug/test_net.exe
```
Expected: both build; `All netlib tests passed.` and `All net tests passed.` (engine still green via the interim bridge; the stress/echo tests are the concurrency safety net for the Send rework).

- [ ] **Step 8: Commit**
```
git add src/netlib/include/netlib/IIo.h src/netlib/src/IocpCore.h src/netlib/src/IocpCore.cpp src/netlib/src/TcpClient.cpp src/netlib/src/TcpServer.cpp src/netlib/src/InMemoryAdapter.cpp src/engine/src/network/NetSubsystem.cpp tests/test_netlib.cpp
git commit -m "refactor(net): netlib Send takes OwnedBuffer&&, length written in place"
```

---

### Task 3: Recv directly into the Framer buffer

**Files:**
- Modify: `src/netlib/src/Framer.h` (add `PrepareRecv`/`CommitRecv`; keep `Push`)
- Modify: `src/netlib/src/Framer.cpp` (shared scan/compact; recv-in-place path)
- Modify: `src/netlib/src/IocpCore.h` (drop `IoOp::recvBuffer`)
- Modify: `src/netlib/src/IocpCore.cpp` (`PostRecv`/`HandleRecv` use the framer buffer)

- [ ] **Step 1: Extend the Framer interface**

In `src/netlib/src/Framer.h`, add to the public section (keep `Push` for the unit test + any span feeders):
```cpp
    // Recv-in-place API: reserve `want` bytes at the buffer tail and return a write
    // pointer; the caller WSARecv's into it, then calls CommitRecv(got, onFrame).
    // Eliminates the separate recv-scratch + insert copy.
    std::byte* PrepareRecv(size_t want);
    // Advance the buffer by `got` received bytes, emit every complete frame, compact
    // the consumed prefix. Returns false on an oversize length prefix (disconnect).
    bool CommitRecv(size_t got, const OnFrame& onFrame);
```
Add a private member to track the committed (valid) size separately from the buffer's capacity:
```cpp
    size_t m_Size = 0;   // valid bytes in m_Buf (m_Buf may have extra reserved tail capacity)
```

- [ ] **Step 2: Implement the recv-in-place path + share the scan**

In `src/netlib/src/Framer.cpp`, refactor so `Push`, `PrepareRecv`, and `CommitRecv` share one scan/compact. Replace the file body with:
```cpp
#include "Framer.h"

#include <cstring>

#include "lib.h"  // SM_WARN

namespace netlib::detail {

// Scan [0, m_Size) for complete [u32 len][payload] frames, emit each, compact the
// consumed prefix. Returns false on an oversize length prefix.
bool Framer::ScanAndCompact(const OnFrame& onFrame) {
    size_t offset = 0;
    for (;;) {
        if (m_Size - offset < 4) break;
        uint32_t len = 0;
        std::memcpy(&len, m_Buf.data() + offset, 4);   // x64 LE
        if (len > m_Max) {
            SM_WARN("netlib: frame length %u exceeds max %u; disconnecting", len, m_Max);
            return false;
        }
        if (m_Size - offset - 4 < len) break;
        const std::byte* payload = m_Buf.data() + offset + 4;
        onFrame(std::span<const std::byte>(payload, len));
        offset += 4 + len;
    }
    if (offset > 0) {
        const size_t remaining = m_Size - offset;
        if (remaining > 0) std::memmove(m_Buf.data(), m_Buf.data() + offset, remaining);
        m_Size = remaining;
    }
    return true;
}

bool Framer::Push(std::span<const std::byte> in, const OnFrame& onFrame) {
    std::byte* dst = PrepareRecv(in.size());
    if (!in.empty()) std::memcpy(dst, in.data(), in.size());
    return CommitRecv(in.size(), onFrame);
}

std::byte* Framer::PrepareRecv(size_t want) {
    if (m_Buf.size() < m_Size + want) m_Buf.resize(m_Size + want);
    return m_Buf.data() + m_Size;
}

bool Framer::CommitRecv(size_t got, const OnFrame& onFrame) {
    m_Size += got;
    return ScanAndCompact(onFrame);
}

} // namespace netlib::detail
```
In `Framer.h`, declare the private helper: `bool ScanAndCompact(const OnFrame& onFrame);`. (`m_Buf` stays; `m_Size` is the valid-byte count; the buffer keeps reserved tail capacity across recvs, eliminating the per-recv 64 KB allocation.)

- [ ] **Step 3: Point WSARecv at the framer buffer; drop the recv scratch**

In `src/netlib/src/IocpCore.h`, remove `std::vector<std::byte> recvBuffer;` from `IoOp` (send keeps `OwnedBuffer sendBuffer;`). Add a recv size constant near the top of `IocpCore.cpp` if not present:
```cpp
namespace { constexpr size_t kRecvChunk = 64 * 1024; }
```
In `src/netlib/src/IocpCore.cpp` `PostRecv`, target the framer tail:
```cpp
void IocpCore::PostRecv(const std::shared_ptr<Conn>& c) {
    if (c->closing.load(std::memory_order_acquire)) return;
    auto* op = new IoOp();
    op->type = IoOp::Type::Recv;
    op->conn = c;
    std::byte* dst = c->framer.PrepareRecv(kRecvChunk);   // reserve tail; one recv in flight per conn
    op->wsabuf.buf = reinterpret_cast<CHAR*>(dst);
    op->wsabuf.len = static_cast<ULONG>(kRecvChunk);
    const SOCKET h = c->sock.load(std::memory_order_acquire);
    if (h == INVALID_SOCKET) { delete op; return; }
    DWORD flags = 0, bytes = 0;
    const int rc = WSARecv(h, &op->wsabuf, 1, &bytes, &flags, &op->ov, nullptr);
    if (rc == SOCKET_ERROR) {
        const int err = WSAGetLastError();
        if (err != WSA_IO_PENDING) {
            if (err != WSAECONNRESET && err != WSAECONNABORTED)
                SM_WARN("netlib: WSARecv failed (%d)", err);
            delete op;
            Disconnect(c);
        }
    }
}
```
In `HandleRecv`, commit into the framer instead of pushing a span:
```cpp
void IocpCore::HandleRecv(const std::shared_ptr<Conn>& c, IoOp* op, DWORD bytes, bool ok) {
    if (!ok || bytes == 0) { delete op; Disconnect(c); return; }
    const bool framerOk = c->framer.CommitRecv(
        bytes, [&](std::span<const std::byte> frame) { Emit(c->id, IoEvent::Kind::Message, frame); });
    delete op;
    if (!framerOk) { Disconnect(c); return; }
    PostRecv(c);
}
```
**Invariant to preserve:** exactly one recv is outstanding per `Conn` (PostRecv is called once at Register and once per HandleRecv), so the framer buffer is only grown/written by that conn's single in-flight recv — the `PrepareRecv` pointer stays valid for the WSARecv duration. Do not post multiple concurrent recvs per conn.

- [ ] **Step 4: Build + run netlib AND engine tests**

Run:
```
cmake --build out/build/msvc-win64-vs2026-enterprise --target test_netlib
./out/build/msvc-win64-vs2026-enterprise/bin/Debug/test_netlib.exe
cmake --build out/build/msvc-win64-vs2026-enterprise --target test_net
./out/build/msvc-win64-vs2026-enterprise/bin/Debug/test_net.exe
```
Expected: both green. `test_framer` (drip-feed, coalesced, oversize) still passes via `Push`→`PrepareRecv`/`CommitRecv`; the TCP stress/echo/roundtrip/oversize tests pass over the new recv path.

- [ ] **Step 5: Commit**
```
git add src/netlib/src/Framer.h src/netlib/src/Framer.cpp src/netlib/src/IocpCore.h src/netlib/src/IocpCore.cpp
git commit -m "perf(net): WSARecv directly into the framer buffer (no accumulate copy/alloc)"
```

---

### Task 4: Engine reshape — send pool, agnostic API, recv block-expose, demo port

This task changes the public `NetServices` API (drops `opcode`, adds `AcquireSend`/`Send(SendBuffer)`), so its callers — `NetSubsystem`, `NetServicesImpl`, `test_net.cpp`, and the two demo systems in `game.cpp` — all migrate together to keep the build green.

**Files:**
- Modify: `src/common/include/NetBufferPool.h` (`IndexOf`)
- Modify: `src/common/include/NetServices.h` (`SendBuffer`, API, `NetEvent`)
- Modify: `src/engine/src/network/NetSubsystem.h` (decls, send pool, borrow index)
- Modify: `src/engine/src/network/NetSubsystem.cpp` (AcquireSend/Send/AbortSend, OnAdapterEvent, PollEvent)
- Modify: `src/engine/src/network/NetServicesImpl.cpp` (forwarders)
- Modify: `tests/test_net.cpp` (migrate to the agnostic API; tag-in-payload)
- Modify: `src/game/src/game.cpp` (port `NetServerSystem`/`NetClientSystem` to the new API; game-owned 2-byte tag)

- [ ] **Step 1: Add `NetBufferPool::IndexOf`**

In `src/common/include/NetBufferPool.h`, add a public method (block must be a block start returned by `Block`/`Acquire`):
```cpp
    // Index of a block pointer previously returned by Acquire/Block. UB if `block`
    // is not a block start from this pool.
    uint32_t IndexOf(const std::byte* block) const {
        return static_cast<uint32_t>(
            (block - m_Storage.data()) / static_cast<std::ptrdiff_t>(m_BlockSize));
    }
```

- [ ] **Step 2: New `NetServices` contract (drop opcode, add SendBuffer)**

In `src/common/include/NetServices.h`:
- Add the `SendBuffer` struct (POD; crosses the DLL boundary by value):
```cpp
// A writable send buffer handed to the caller by AcquireSend. The caller serializes
// directly into `data` (up to `cap` bytes), then passes it to Send. `token` is an
// opaque engine handle (do not interpret). Engine reserves a length head before `data`.
struct SendBuffer {
    uint8_t* data  = nullptr;   // writable payload region
    uint32_t cap   = 0;         // bytes available at `data`
    uint64_t token = 0;         // opaque; identifies the backing block/heap for Send/AbortSend
};
```
- In `NetEvent`, **remove** the `uint16_t opcode = 0;` line. Keep `payload`/`len` (now the opaque frame). Update the comment to "opaque payload frame (no opcode — type tagging is the caller's concern)".
- Replace the `Send` function pointer and add `AcquireSend`/`AbortSend` in the `NetServices` struct:
```cpp
    // Acquire a writable send buffer sized for `payloadBytes`. Caller serializes into
    // SendBuffer::data, then calls Send (or AbortSend to discard). GameThread-only.
    SendBuffer (*AcquireSend)(size_t payloadBytes);
    // Send a previously-acquired buffer to a connection (consumes it). `payloadLen`
    // is the number of bytes actually written (<= the acquired cap). Client adapters
    // pass kNetConnInvalid. Returns false on unknown handle (buffer is still released).
    bool (*Send)(NetHandle h, NetConnId conn, SendBuffer buf, uint32_t payloadLen);
    // Discard an acquired buffer without sending (releases the backing block/heap).
    void (*AbortSend)(SendBuffer buf);
```
(Remove the old `bool (*Send)(NetHandle, NetConnId, uint16_t, const uint8_t*, size_t);`.)

- [ ] **Step 3: NetSubsystem decls + send pool**

In `src/engine/src/network/NetSubsystem.h`:
- Add a second pool + the inbound borrow index near `m_Pool`:
```cpp
    std::unique_ptr<NetBufferPool> m_SendPool;     // outbound send buffers (16 KB blocks)
    uint32_t m_BorrowedIndex = UINT32_MAX;         // inbound block lent to the game until next PollEvent
    static constexpr size_t kSendBlockSize = 16 * 1024;
    static constexpr size_t kSendBlocks    = 1024;
```
- Replace the `Send` method decl and add the new ones:
```cpp
    SendBuffer AcquireSend(size_t payloadBytes);
    bool       Send(NetHandle h, NetConnId conn, SendBuffer buf, uint32_t payloadLen);
    void       AbortSend(SendBuffer buf);
```
- Drop the old `bool Send(NetHandle, NetConnId, uint16_t, const uint8_t*, size_t);`.

- [ ] **Step 4: NetSubsystem implementation**

In `src/engine/src/network/NetSubsystem.cpp`:

`Init`: allocate the send pool (and keep `m_DrainBuf` removal for Step 5; in this step `Init` adds the send pool):
```cpp
void NetSubsystem::Init() {
    Shutdown();
    m_Ring = std::make_unique<MpscRing<RingEvent, kRingSize>>();
    m_Pool = std::make_unique<NetBufferPool>(kBlockSize, kBlocks);
    m_SendPool = std::make_unique<NetBufferPool>(kSendBlockSize, kSendBlocks);
    m_BorrowedIndex = UINT32_MAX;
}
```
`Shutdown`: add `m_SendPool.reset();` next to `m_Pool.reset();`.

Add the send-token encoding helpers + the three methods (replace the interim `Send` from Task 2). The token high bit marks a pool block; low bits hold the pool index; a heap buffer uses `kHeapToken`:
```cpp
namespace {
    constexpr uint64_t kPoolBit   = 1ull << 63;
    constexpr uint64_t kHeapToken = 0;           // token == 0 => heap-backed
    // OwnedBuffer deleters (run on the IOCP worker at send completion).
    void ReleaseToSendPool(void* poolCtx, std::byte* base) noexcept {
        auto* pool = static_cast<NetBufferPool*>(poolCtx);
        pool->Release(pool->IndexOf(base));      // base == block start (head at offset 0)
    }
    void FreeHeap(void*, std::byte* base) noexcept { delete[] base; }
}

SendBuffer NetSubsystem::AcquireSend(size_t payloadBytes) {
    SendBuffer sb{};
    const size_t total = netlib::OwnedBuffer::kHeadBytes + payloadBytes;
    if (m_SendPool && total <= m_SendPool->BlockSize()) {
        uint32_t idx;
        std::byte* block = m_SendPool->Acquire(idx);
        if (block) {
            sb.data  = reinterpret_cast<uint8_t*>(block + netlib::OwnedBuffer::kHeadBytes);
            sb.cap   = static_cast<uint32_t>(payloadBytes);
            sb.token = kPoolBit | idx;
            return sb;
        }
        // pool momentarily exhausted -> fall through to heap
    }
    auto* base = new std::byte[total];
    sb.data  = reinterpret_cast<uint8_t*>(base + netlib::OwnedBuffer::kHeadBytes);
    sb.cap   = static_cast<uint32_t>(payloadBytes);
    sb.token = kHeapToken;   // heap; base recovered as data-kHeadBytes
    return sb;
}

void NetSubsystem::AbortSend(SendBuffer buf) {
    if (!buf.data) return;
    std::byte* base = reinterpret_cast<std::byte*>(buf.data) - netlib::OwnedBuffer::kHeadBytes;
    if (buf.token & kPoolBit) { if (m_SendPool) m_SendPool->Release(static_cast<uint32_t>(buf.token & ~kPoolBit)); }
    else                        delete[] base;
}

bool NetSubsystem::Send(NetHandle h, NetConnId conn, SendBuffer buf, uint32_t payloadLen) {
    if (!buf.data) return false;
    std::byte* base = reinterpret_cast<std::byte*>(buf.data) - netlib::OwnedBuffer::kHeadBytes;

    netlib::OwnedBuffer owned =
        (buf.token & kPoolBit)
            ? netlib::OwnedBuffer(base, payloadLen, m_SendPool.get(), &ReleaseToSendPool)
            : netlib::OwnedBuffer(base, payloadLen, nullptr, &FreeHeap);

    std::scoped_lock lk(m_Mx);
    auto it = m_Adapters.find(static_cast<uint32_t>(h));
    if (it == m_Adapters.end()) return false;   // `owned` destructs -> deleter reclaims
    if (it->second.server) { it->second.server->Send(netlib::ConnId{ conn }, std::move(owned)); return true; }
    if (it->second.client) { it->second.client->Send(std::move(owned)); return true; }
    return false;
}
```
Keep the `namespace { constexpr uint32_t kOpcodeBytes = 2; }` only if still referenced; after this task the engine no longer prepends an opcode, so **remove** `kOpcodeBytes` and the opcode handling in `OnAdapterEvent` (Step 5).

- [ ] **Step 5: OnAdapterEvent + PollEvent — opaque frame, expose pool block**

In `OnAdapterEvent`, the inbound `Message` branch must stop stripping a `[u16 opcode]` (the engine is now agnostic) — copy the WHOLE frame into the pool block:
```cpp
        case netlib::IoEvent::Kind::Message: {
            re.kind = NetEventKind::Message;
            const auto& p = ev.payload;
            const size_t payloadLen = p.size();
            if (payloadLen > m_Pool->BlockSize()) { SM_WARN("NetSubsystem: payload %zu > block %zu; dropped", payloadLen, m_Pool->BlockSize()); return; }
            uint32_t idx;
            std::byte* block = m_Pool->Acquire(idx);
            if (!block) { SM_WARN("NetSubsystem: pool exhausted; message dropped"); return; }
            if (payloadLen) std::memcpy(block, p.data(), payloadLen);
            re.poolIndex = idx;
            re.len = static_cast<uint32_t>(payloadLen);
            re.hasPayload = true;
            break;
        }
```
Remove `re.opcode` usage (the `RingEvent` struct's `opcode` field can be deleted too — remove `uint16_t opcode;` from `RingEvent` in `NetSubsystem.h` and any reference).

Replace `PollEvent` to release the previously-borrowed block and expose the pool block directly (drop `m_DrainBuf`):
```cpp
bool NetSubsystem::PollEvent(NetEvent* out) {
    if (!out || !m_Ring) return false;
    // Release the block borrowed by the PREVIOUS PollEvent (the game has had its turn).
    if (m_BorrowedIndex != UINT32_MAX) { m_Pool->Release(m_BorrowedIndex); m_BorrowedIndex = UINT32_MAX; }

    RingEvent re{};
    if (!m_Ring->Dequeue(re)) return false;
    out->kind    = re.kind;
    out->adapter = re.adapter;
    out->conn    = re.conn;
    out->payload = nullptr;
    out->len     = 0;
    if (re.hasPayload) {
        out->payload = m_Pool->Block(re.poolIndex);   // borrowed until next PollEvent
        out->len     = re.len;
        m_BorrowedIndex = re.poolIndex;               // released at the top of the next PollEvent
    }
    return true;
}
```
Remove the `m_DrainBuf` member from `NetSubsystem.h` and the `m_DrainBuf.resize(...)` in `Init`. **Note the contract change:** the borrowed block is freed at the next `PollEvent` — a caller draining in a `while (PollEvent(&ev))` loop must finish using `ev.payload` before the next iteration (it already does: each event is consumed in the loop body). On `Shutdown`, also release a still-borrowed block: add `if (m_BorrowedIndex != UINT32_MAX && m_Pool) { m_Pool->Release(m_BorrowedIndex); m_BorrowedIndex = UINT32_MAX; }` at the start of `Shutdown` (before `m_Pool.reset()`).

`out->payload` is `const uint8_t*` per `NetServices.h`; `m_Pool->Block` returns `std::byte*` — cast: `out->payload = reinterpret_cast<const uint8_t*>(m_Pool->Block(re.poolIndex));`.

- [ ] **Step 6: Forwarders**

In `src/engine/src/network/NetServicesImpl.cpp`, replace the `FwdSend` and wire the new pointers:
```cpp
SendBuffer FwdAcquireSend(size_t n) { return NetSubsystem::Instance().AcquireSend(n); }
bool       FwdSend(NetHandle h, NetConnId conn, SendBuffer b, uint32_t n) { return NetSubsystem::Instance().Send(h, conn, b, n); }
void       FwdAbortSend(SendBuffer b) { NetSubsystem::Instance().AbortSend(b); }
```
```cpp
    out.AcquireSend = &FwdAcquireSend;
    out.Send        = &FwdSend;
    out.AbortSend   = &FwdAbortSend;
```
(Remove the old `FwdSend` opcode signature + its assignment.)

- [ ] **Step 7: Migrate test_net.cpp to the agnostic API**

In `tests/test_net.cpp`:
- `test_net_types`: remove the `ev.opcode = 7;` / `ev.opcode == 7` assertions; replace with a `payload`/`len` field check, e.g.:
```cpp
    NetEvent ev{};
    ev.kind = NetEventKind::Message;
    ev.len  = 5;
    CHECK(ev.kind == NetEventKind::Message && ev.len == 5, "types: NetEvent fields");
    CHECK(NetHandle::Invalid == NetHandle{0}, "types: NetHandle::Invalid is 0");
```
- Add a small game-style tag helper at file scope (mirrors what a game does — 2-byte tag inside the payload):
```cpp
// Send helper: writes a [u16 tag] + body into an acquired SendBuffer, then Sends.
static bool send_tagged(NetServices& net, NetHandle h, NetConnId conn, uint16_t tag, const std::vector<uint8_t>& body) {
    SendBuffer sb = net.AcquireSend(2 + body.size());
    if (!sb.data) return false;
    sb.data[0] = static_cast<uint8_t>(tag & 0xff);
    sb.data[1] = static_cast<uint8_t>((tag >> 8) & 0xff);
    std::memcpy(sb.data + 2, body.data(), body.size());
    return net.Send(h, conn, sb, static_cast<uint32_t>(2 + body.size()));
}
// Read the [u16 tag] from a received frame.
static uint16_t frame_tag(const NetEvent& ev) {
    return static_cast<uint16_t>(ev.payload[0]) | (static_cast<uint16_t>(ev.payload[1]) << 8);
}
```
- `test_netsub_roundtrip`: replace the two `net.Send(..., opcode, data, len)` calls with `send_tagged(net, cli, kNetConnInvalid, 42, u8("hello"))` and `send_tagged(net, srv, serverConn, 7, u8("yo"))`; replace the recv checks to read the tag from the payload:
```cpp
            if (ev.adapter == srv && ev.kind == NetEventKind::Message) {
                if (ev.len == 7 && frame_tag(ev) == 42 && std::memcmp(ev.payload + 2, "hello", 5) == 0) gotClientMsg = true;
            }
```
  and similarly for the server reply (`ev.len == 4 && frame_tag(ev) == 7 && memcmp(ev.payload+2,"yo",2)==0`). (Frame length = 2 tag + body.)
- `test_netsub_shutdown_while_connected` and `test_netsub_traffic_then_shutdown`: replace `net.Send(cli, kNetConnInvalid, 1, m.data(), m.size())` with `send_tagged(net, cli, kNetConnInvalid, 1, m)`, and the server echo `net.Send(srv, ev.conn, 2, ev.payload, ev.len)` with re-acquiring + copying the received frame back:
```cpp
            if (ev.adapter == srv && ev.kind == NetEventKind::Message) {
                std::vector<uint8_t> body(ev.payload, ev.payload + ev.len);   // copy before next PollEvent
                SendBuffer sb = net.AcquireSend(body.size());
                if (sb.data) { std::memcpy(sb.data, body.data(), body.size()); net.Send(srv, ev.conn, sb, (uint32_t)body.size()); }
            }
```
  (The echo copies the frame verbatim — tag included — back; that's fine for the crash/shutdown stress intent.)
- Add `#include <cstring>` (already present) and ensure `<vector>` is included (it is).

- [ ] **Step 8: Port the demo systems to the agnostic API**

In `src/game/src/game.cpp`, `NetServerSystem` and `NetClientSystem` currently call `net->Send(handle, conn, opcode, payload, len)` and read `ev.opcode`. Port them to a game-owned 2-byte tag inside the payload (NOT protobuf — that is the deferred follow-up). Add a file-local helper near the top of the anonymous namespace or just above the systems:
```cpp
namespace {
// Game-owned 2-byte message tag carried INSIDE the opaque frame (the engine no
// longer has an opcode). Mirrors what the future protobuf codec will do.
constexpr uint16_t kTagPing = 1;
constexpr uint16_t kTagPong = 2;

bool SendTagged(const NetServices* net, NetHandle h, NetConnId conn, uint16_t tag,
                const uint8_t* body, uint32_t bodyLen) {
    SendBuffer sb = net->AcquireSend(2u + bodyLen);
    if (!sb.data) return false;
    sb.data[0] = static_cast<uint8_t>(tag & 0xff);
    sb.data[1] = static_cast<uint8_t>((tag >> 8) & 0xff);
    if (bodyLen) std::memcpy(sb.data + 2, body, bodyLen);
    return net->Send(h, conn, sb, 2u + bodyLen);
}
uint16_t FrameTag(const NetEvent& ev) {
    return ev.len >= 2 ? (static_cast<uint16_t>(ev.payload[0]) |
                          (static_cast<uint16_t>(ev.payload[1]) << 8)) : 0;
}
} // namespace
```
- `NetServerSystem::Update`: replace the opcode-1 echo. The poll loop becomes:
```cpp
        NetEvent ev{};
        while (net->PollEvent(&ev)) {
            if (ev.adapter != m_Server) continue;
            if (ev.kind == NetEventKind::Message && FrameTag(ev) == kTagPing) {
                const uint8_t* body = ev.payload + 2; const uint32_t bodyLen = ev.len - 2;
                SendTagged(net, m_Server, ev.conn, kTagPong, body, bodyLen);   // echo as pong
                SM_TRACE("NetDemo[server]: echoed ping from conn %llu", (unsigned long long)ev.conn);
            } else if (ev.kind == NetEventKind::Connected) {
                SM_TRACE("NetDemo[server]: client connected (conn %llu)", (unsigned long long)ev.conn);
            }
        }
```
- `NetClientSystem::Update`: replace the recv check and the ping send. Recv branch:
```cpp
            } else if (ev.kind == NetEventKind::Message && FrameTag(ev) == kTagPong) {
                SM_TRACE("NetDemo[client]: got echo (%u bytes)", ev.len);
            } else if (...Disconnected/Error as before...) { ... }
```
  Ping send (the `net->Send(m_Client, kNetConnInvalid, 1, payload, sizeof(payload))` line):
```cpp
                const uint8_t payload[4] = { 'p','i','n','g' };
                SendTagged(net, m_Client, kNetConnInvalid, kTagPing, payload, sizeof(payload));
                SM_TRACE("NetDemo[client]: sent ping (tag 1)");
```
Ensure `<cstring>` is included in `game.cpp` (it is, via existing includes — verify; add `#include <cstring>` if missing).

- [ ] **Step 9: Build everything + run both test suites**

Run:
```
cmake --build out/build/msvc-win64-vs2026-enterprise --target test_net
./out/build/msvc-win64-vs2026-enterprise/bin/Debug/test_net.exe
cmake --build out/build/msvc-win64-vs2026-enterprise --target test_netlib
./out/build/msvc-win64-vs2026-enterprise/bin/Debug/test_netlib.exe
cmake --build out/build/msvc-win64-vs2026-enterprise --target game
cmake --build out/build/msvc-win64-vs2026-enterprise --target editor
```
Expected: `All net tests passed.`, `All netlib tests passed.`, and `game` + `editor` link clean. (The demo systems now use the agnostic API; the engine carries no opcode.)

- [ ] **Step 10: Commit**
```
git add src/common/include/NetBufferPool.h src/common/include/NetServices.h src/engine/src/network/NetSubsystem.h src/engine/src/network/NetSubsystem.cpp src/engine/src/network/NetServicesImpl.cpp tests/test_net.cpp src/game/src/game.cpp
git commit -m "refactor(net): agnostic SendBuffer API + send pool + expose recv block"
```

---

### Task 5: (Optional) Net pool stats in the Memory panel

**Files:**
- Modify: `src/engine/src/network/NetSubsystem.cpp` / `.h` (register pools or surface stats)

Only do this if it does not complicate Task 4. `NetBufferPool` is not an `Engine::IAllocator` and is off the Memory panel. Surfacing it cleanly requires either an adapter exposing `AllocatorStats` or registering with `AllocatorRegistry`.

- [ ] **Step 1: Decide feasibility**

Read `src/engine/include/memory/AllocatorRegistry.h` and `AllocatorStats.h`. If a lightweight `IAllocator`-less stats hook exists (e.g. the registry accepts a name + a stats provider), expose `m_Pool`/`m_SendPool` block usage (`BlockCount`, in-use count). If it would require making `NetBufferPool` an `IAllocator` (cross-thread, non-trivial), **stop and report DONE_WITH_CONCERNS** noting it's deferred — do not force it.

- [ ] **Step 2: If feasible, implement + verify in the editor Memory panel; else skip**

If implemented: build `editor`, confirm the panel lists the net pools. Commit:
```
git add src/engine/src/network/NetSubsystem.h src/engine/src/network/NetSubsystem.cpp
git commit -m "feat(net): surface net buffer pool usage in the Memory panel"
```

---

## Self-Review

**Spec coverage:**
- Agnostic contract / opcode removed → Task 4 (Steps 2, 5, 7, 8).
- Send `AcquireSend` + serialize-in-place + move + `OwnedBuffer` deleter → Tasks 1, 2, 4.
- Send copies 2+3 eliminated → Task 2 (length-in-place) + Task 4 (no opcode prepend, pool buffer).
- Recv into framer buffer (kills copy B + 64 KB alloc) → Task 3.
- Recv expose pool block (kills copy D, drop `m_DrainBuf`) → Task 4 Step 5.
- Keep copy C → unchanged (Task 4 Step 5 still copies the frame into the inbound pool block).
- Reuse `NetBufferPool` both directions; new 16 KB send instance; heap >16 KB → Task 4 Steps 1, 3, 4.
- Engine single-threaded allocators not used → respected (only `NetBufferPool` touched).
- Memory-panel stats (optional) → Task 5.
- Game codec/protobuf wiring out of scope → respected (Task 4 Step 8 uses a raw game-owned tag, no protobuf).

**Placeholder scan:** none — every step has concrete code/commands.

**Type consistency:** `OwnedBuffer` (`Data`/`Payload`/`PayloadLen`/`TotalSize`/`kHeadBytes`), `SendBuffer` (`data`/`cap`/`token`), `NetSubsystem::{AcquireSend,Send,AbortSend}`, `NetBufferPool::IndexOf`, token bits (`kPoolBit`/`kHeapToken`), and the `[u16 tag]` helpers (`SendTagged`/`FrameTag`/`send_tagged`/`frame_tag`) are consistent across tasks. `NetEvent::opcode` removed everywhere it was read (Task 4 Steps 2, 7, 8).

**Build-green staging:** verified — Task 2 keeps the engine compiling via the interim heap-`OwnedBuffer` bridge; Task 3 is netlib-internal; Task 4 migrates the API + all callers (engine, tests, demo) atomically. Tasks 1–3 verify `test_netlib` (+ `test_net` from Task 2 on); Task 4 verifies both suites + `game` + `editor`.

**Concurrency safety nets:** the existing `test_netlib` stress/echo/oversize/roundtrip/multiconn and `test_net` mpsc/pool concurrency + shutdown-under-traffic tests cover the Send rework (Task 2), the recv-into-framer rework (Task 3), and the pool/borrow changes (Task 4). Each task reruns them.
