# netlib Engine Integration (Phase 2) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Wire `netlib` into the engine — a `NetServices` fn-ptr bridge (like `NavServices`) backed by a `NetSubsystem` that owns a lock-free MPSC inbound ring + a lock-free `NetBufferPool`, is the `IIoSink` adapter threads push into, and exposes create/send/poll/close to `Game.dll` with zero `Engine.dll` link — plus Model-B hot-reload teardown.

**Architecture:** Adapters (netlib) own their threads and push `IoEvent`s into per-adapter sinks owned by the singleton `NetSubsystem`; the sink copies the payload into a lock-free fixed-block pool and enqueues a POD `NetEvent` onto one lock-free Vyukov MPSC ring; GameThread drains it via `NetServices::PollEvent` (the bridge threaded through `SystemContext`, mirroring `NavServices`). Outbound `Send` prepends a `uint16` opcode and calls the adapter directly. The engine owns **no** IO threads.

**Tech Stack:** C++23, lock-free Vyukov bounded MPMC ring (used MPSC + as the pool free-list), `netlib.dll` (Phase 1), the `NavServices`/`NavServicesImpl` bridge pattern, `SystemContext` per-tick threading in `GameThread`.

**Spec:** `docs/superpowers/specs/2026-05-30-net-engine-integration-design.md` (+ umbrella `2026-05-30-networking-architecture-design.md`).

**Prereq:** Phase 1 merged on this branch (`netlib.dll`: `IIoSink`/`IIoClient`/`IIoServer`, `MakeTcpServer`/`MakeTcpClient`/`MakeInMemoryPair`, `Endpoint`/`ConnId`/`ConnConfig`, `IoEvent`). Latest branch commit at plan time: `4e0b408`.

---

## Conventions for every task

- **Build engine + tests:**
  ```
  cmake --preset msvc-win64-vs2026-community
  cmake --build --preset msvc-win64-vs2026-community --target test_net
  ```
  Engine-code changes (NetSubsystem, GameThread, SystemContext) require a full editor restart to see live, but the **automated proof is `test_net`** — a standalone exe linking `Engine` + `netlib` (mirrors how `test_navmesh` links `Engine`). Live in-editor behavior is a manual smoke (Task 5), not a blocking gate.
- **Run tests:** `./out/build/msvc-win64-vs2026-community/bin/Debug/test_net.exe` → success prints `All net tests passed.`
- **Test harness style:** the `CHECK(cond,msg)` + `main`-prints-summary pattern from `tests/test_netlib.cpp`/`tests/test_navmesh.cpp`. Define a local `platform_debug_break` stub (Engine exports its own, but link order — copy the stub from `tests/test_netlib.cpp` to be safe; if it causes a duplicate-symbol link error because Engine already provides it, drop the stub).
- **Logging:** header-only `SM_TRACE/SM_WARN/SM_ERROR`. Never silent-drop — log on backpressure/pool-exhaustion (`feedback_logging_over_silent_skip`). No `SM_ASSERT` in the lock-free primitives.
- **Commit author:** `Nuno Silva <nuno.levezinho@live.com.pt>`. Never `--no-verify`. Branch `feat/networking-design`.

---

## File Structure

```
src/common/include/
  MpscRing.h          NEW — lock-free Vyukov bounded MPMC ring (used MPSC + pool free-list). Header-only.
  NetBufferPool.h     NEW — lock-free fixed-block payload pool (built on MpscRing<uint32_t>). Header-only.
  NetServices.h       NEW — game-facing types (NetHandle, NetConnId, NetEvent, NetClientConfig,
                            NetServerConfig) + the NetServices fn-ptr table. Includes netlib public headers.
  Systems.h           MODIFY — add `const NetServices* Net = nullptr;` to SystemContext.
src/engine/src/network/
  NetSubsystem.h      NEW — singleton: registry, per-adapter sinks, MPSC ring, NetBufferPool, opcode framing.
  NetSubsystem.cpp    NEW
  NetServicesImpl.h   NEW — ENGINE_API Init(NetServices&) (mirrors NavServicesImpl.h)
  NetServicesImpl.cpp NEW — forwards the table to NetSubsystem::Instance()
src/engine/src/threading/
  GameThread.cpp      MODIFY — init NetServices, thread &netServices through SystemContext, Model-B teardown.
src/engine/CMakeLists.txt   MODIFY — add network sources + include dir + link netlib.
src/common/CMakeLists.txt   MODIFY — add netlib include dir to CommonHeaders INTERFACE.
tests/CMakeLists.txt        MODIFY — add test_net target.
tests/test_net.cpp          NEW — MpscRing, NetBufferPool, NetSubsystem TCP-loopback round-trip, teardown tests.
```

**Design decisions locked here:**
- `NetServices.h` lives in `common/include` (like `NavServices.h`) and **includes netlib's header-only public headers** (`<netlib/netlib.h>` → Endpoint/IoEvent/IIo + factories). To let every consumer of `Systems.h` compile, add netlib's include dir to the `CommonHeaders` INTERFACE. Including headers ≠ linking — only targets that *call* netlib symbols (Engine, test_net, later game) link `netlib`.
- **One singleton `NetSubsystem`** (matches `NavMeshSystem`; the fn-ptr table calls `Instance()`). Documented Phase-4 de-singletoning caveat. Tests reset it via `Init()`/`Shutdown()`.
- **Opcode framing:** the engine sits between the game (opcode+payload) and netlib's length-prefix framing. `Send` prepends `[uint16 LE opcode]`; inbound splits the first 2 bytes of each netlib frame back into `opcode` + `payload`. So the wire frame is `[uint32 len][uint16 opcode][payload]` (len includes the opcode).
- **Per-adapter sink:** `IIoSink::OnIoEvent` carries no adapter id, so `NetSubsystem` gives each adapter its own tiny `AdapterSink` that tags events with the owning `NetHandle`.
- **Inbound payload lifetime:** the ring element stores `{kind, handle, conn, opcode, poolIndex, len}` (POD). `PollEvent` copies the pool block into a single reusable drain buffer (GameThread is the sole consumer), releases the block, and points `NetEvent::payload` at the drain buffer (valid until the next `PollEvent`). The game copies if it needs to retain.

---

## Task 1: Lock-free MPSC ring (`MpscRing.h`)

A bounded Vyukov MPMC queue (safe for our MPSC use: many adapter threads enqueue, GameThread dequeues; also reused MPMC for the pool free-list). Header-only.

**Files:**
- Create: `src/common/include/MpscRing.h`
- Create: `tests/test_net.cpp`
- Modify: `tests/CMakeLists.txt` (add `test_net` target)

- [ ] **Step 1: Write the failing test + harness**

`tests/test_net.cpp`:
```cpp
#include <cstdio>
#include <atomic>
#include <thread>
#include <vector>

#include "MpscRing.h"

void platform_debug_break(const char*, const char*, int, const char*) {}

static int g_Failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { std::printf("FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); ++g_Failures; } } while (0)

static void test_mpsc_basic() {
    MpscRing<int, 4> r;
    int out = 0;
    CHECK(!r.Dequeue(out), "mpsc: empty dequeue false");
    CHECK(r.Enqueue(1), "mpsc: enqueue 1");
    CHECK(r.Enqueue(2), "mpsc: enqueue 2");
    CHECK(r.Enqueue(3), "mpsc: enqueue 3");
    CHECK(r.Enqueue(4), "mpsc: enqueue 4 (full ring of 4 holds 4)");
    CHECK(!r.Enqueue(5), "mpsc: enqueue 5 fails (full)");
    CHECK(r.Dequeue(out) && out == 1, "mpsc: dequeue 1");
    CHECK(r.Dequeue(out) && out == 2, "mpsc: dequeue 2");
}

static void test_mpsc_concurrent() {
    // K producers each enqueue M items; one consumer must receive exactly K*M, no loss/dup.
    constexpr int K = 8, M = 10000;
    MpscRing<int, 1024> r;
    std::atomic<int> producedTotal{0};
    std::vector<std::thread> producers;
    std::atomic<bool> go{false};
    for (int k = 0; k < K; ++k) {
        producers.emplace_back([&]{
            while (!go.load()) {}
            for (int i = 0; i < M; ++i) {
                while (!r.Enqueue(1)) { std::this_thread::yield(); }   // retry on full
                producedTotal.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    go.store(true);
    int received = 0;
    int out = 0;
    // Drain until we've received K*M (producers retry on full, so the ring never loses).
    while (received < K * M) {
        if (r.Dequeue(out)) received += out;
        else std::this_thread::yield();
    }
    for (auto& t : producers) t.join();
    CHECK(received == K * M, "mpsc: concurrent received exactly K*M");
    CHECK(!r.Dequeue(out), "mpsc: empty after draining all");
}

int main() {
    test_mpsc_basic();
    test_mpsc_concurrent();
    if (g_Failures == 0) { std::printf("All net tests passed.\n"); return 0; }
    std::printf("%d net test(s) FAILED.\n", g_Failures);
    return 1;
}
```

Append to `tests/CMakeLists.txt`:
```cmake
add_executable(test_net
    test_net.cpp
)

target_link_libraries(test_net PRIVATE
    CommonHeaders
    Engine
    netlib
)

target_include_directories(test_net PRIVATE
    ${CMAKE_SOURCE_DIR}/src/common/include
    ${CMAKE_SOURCE_DIR}/src/engine/src
    ${CMAKE_SOURCE_DIR}/src/engine/src/network
    ${CMAKE_SOURCE_DIR}/src/netlib/include
)

set_target_properties(test_net PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    FOLDER Tests
)
```
> `test_net` links `Engine` (for NetSubsystem in later tasks) + `netlib`. The `platform_debug_break` stub: if linking `Engine` causes a duplicate-symbol error (Engine provides its own), delete the stub from `test_net.cpp`. Try with the stub first; if `LNK2005`/duplicate, remove it.

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build --preset msvc-win64-vs2026-community --target test_net`
Expected: FAIL — `MpscRing.h` not found.

- [ ] **Step 3: Implement the ring**

`src/common/include/MpscRing.h`:
```cpp
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>      // std::hardware_destructive_interference_size (fallback to 64)

// Bounded lock-free queue (Dmitry Vyukov's MPMC algorithm). Safe for multiple
// producers AND multiple consumers; we use it MPSC (many adapter threads enqueue,
// GameThread dequeues) and also as a free-list inside NetBufferPool. Capacity N
// must be a power of two; the ring holds up to N elements. No allocation per op.
// T must be trivially copyable (POD).
template <typename T, size_t N>
class MpscRing {
    static_assert((N & (N - 1)) == 0, "N must be a power of two");
    static constexpr size_t kLine = 64;   // cache-line size (avoid false sharing)

    struct alignas(kLine) Cell {
        std::atomic<size_t> seq;
        T                   data;
    };

public:
    MpscRing() {
        for (size_t i = 0; i < N; ++i)
            m_Cells[i].seq.store(i, std::memory_order_relaxed);
        m_Enq.store(0, std::memory_order_relaxed);
        m_Deq.store(0, std::memory_order_relaxed);
    }
    MpscRing(const MpscRing&) = delete;
    MpscRing& operator=(const MpscRing&) = delete;

    // Returns false if full.
    bool Enqueue(const T& v) {
        Cell* cell;
        size_t pos = m_Enq.load(std::memory_order_relaxed);
        for (;;) {
            cell = &m_Cells[pos & (N - 1)];
            const size_t seq = cell->seq.load(std::memory_order_acquire);
            const intptr_t dif = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);
            if (dif == 0) {
                if (m_Enq.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) break;
            } else if (dif < 0) {
                return false;   // full
            } else {
                pos = m_Enq.load(std::memory_order_relaxed);
            }
        }
        cell->data = v;
        cell->seq.store(pos + 1, std::memory_order_release);
        return true;
    }

    // Returns false if empty.
    bool Dequeue(T& out) {
        Cell* cell;
        size_t pos = m_Deq.load(std::memory_order_relaxed);
        for (;;) {
            cell = &m_Cells[pos & (N - 1)];
            const size_t seq = cell->seq.load(std::memory_order_acquire);
            const intptr_t dif = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);
            if (dif == 0) {
                if (m_Deq.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) break;
            } else if (dif < 0) {
                return false;   // empty
            } else {
                pos = m_Deq.load(std::memory_order_relaxed);
            }
        }
        out = cell->data;
        cell->seq.store(pos + N, std::memory_order_release);
        return true;
    }

private:
    alignas(kLine) Cell m_Cells[N];
    alignas(kLine) std::atomic<size_t> m_Enq;
    alignas(kLine) std::atomic<size_t> m_Deq;
};
```

- [ ] **Step 4: Run to verify it passes**

Build + run. Expected: `All net tests passed.` Run the exe 3× (the concurrent test is the real check; must pass every time).

- [ ] **Step 5: Commit**

```bash
git add src/common/include/MpscRing.h tests/test_net.cpp tests/CMakeLists.txt
git commit -m "feat(net): lock-free bounded MPSC ring (Vyukov MPMC)"
```

---

## Task 2: Lock-free `NetBufferPool`

Fixed-block payload pool; acquire/release are lock-free (a free-index queue built on `MpscRing<uint32_t>`). Adapter threads acquire (in the sink), GameThread releases (in PollEvent).

**Files:**
- Create: `src/common/include/NetBufferPool.h`
- Modify: `tests/test_net.cpp`

- [ ] **Step 1: Write the failing tests**

Add to `tests/test_net.cpp` (include + tests + calls in `main`):
```cpp
#include "NetBufferPool.h"
#include <cstring>

static void test_pool_basic() {
    NetBufferPool pool(64, 4);   // 4 blocks of 64 bytes
    uint32_t i0 = 0, i1 = 0;
    std::byte* b0 = pool.Acquire(i0);
    CHECK(b0 != nullptr, "pool: acquire 0 non-null");
    std::byte* b1 = pool.Acquire(i1);
    CHECK(b1 != nullptr && i1 != i0, "pool: acquire 1 distinct index");
    const char* msg = "hi";
    std::memcpy(b0, msg, 3);
    CHECK(std::memcmp(pool.Block(i0), "hi", 3) == 0, "pool: Block(i) round-trips bytes");
    pool.Release(i0);
    pool.Release(i1);
    // After release, capacity is restored.
    uint32_t i2;
    CHECK(pool.Acquire(i2) != nullptr, "pool: reacquire after release");
    pool.Release(i2);
}

static void test_pool_exhaustion() {
    NetBufferPool pool(32, 2);
    uint32_t a, b, c;
    CHECK(pool.Acquire(a) != nullptr, "pool: acq 1");
    CHECK(pool.Acquire(b) != nullptr, "pool: acq 2");
    CHECK(pool.Acquire(c) == nullptr, "pool: acq 3 exhausted -> null");
    pool.Release(a); pool.Release(b);
}

static void test_pool_concurrent() {
    // Many threads acquire+release; balanced, no leak, no double-free observed.
    NetBufferPool pool(64, 256);
    constexpr int K = 8, iters = 20000;
    std::vector<std::thread> ts;
    std::atomic<bool> go{false};
    std::atomic<int> failures{0};
    for (int k = 0; k < K; ++k) ts.emplace_back([&]{
        while (!go.load()) {}
        for (int i = 0; i < iters; ++i) {
            uint32_t idx;
            std::byte* p = pool.Acquire(idx);
            if (p) { p[0] = std::byte{1}; pool.Release(idx); }
            // p==null under contention is fine (pool momentarily empty); not a failure.
        }
    });
    go.store(true);
    for (auto& t : ts) t.join();
    // All blocks must be free again: we can acquire all 256.
    std::vector<uint32_t> held;
    for (int i = 0; i < 256; ++i) { uint32_t idx; if (pool.Acquire(idx)) held.push_back(idx); }
    CHECK(held.size() == 256, "pool: all blocks free after concurrent churn (no leak/double-free)");
    for (uint32_t idx : held) pool.Release(idx);
}
```
Add `test_pool_basic(); test_pool_exhaustion(); test_pool_concurrent();` to `main`.

- [ ] **Step 2: Run to verify it fails**

Build → FAIL: `NetBufferPool.h` not found.

- [ ] **Step 3: Implement the pool**

`src/common/include/NetBufferPool.h`:
```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "MpscRing.h"

// Fixed-block payload pool with lock-free acquire/release. The free list is an
// MPMC ring of block indices. Adapter threads Acquire (copying received bytes in),
// GameThread Releases after PollEvent consumes the block. Acquire returns nullptr
// when momentarily exhausted (caller logs + drops/backpressures — never silently).
//
// Block count is rounded UP to a power of two (ring capacity requirement); the
// extra blocks are simply available capacity.
class NetBufferPool {
public:
    NetBufferPool(size_t blockSize, size_t blockCount)
        : m_BlockSize(blockSize), m_Count(RoundUpPow2(blockCount)) {
        m_Storage.resize(m_BlockSize * m_Count);
        m_Free = std::make_unique<FreeRing>();
        for (uint32_t i = 0; i < static_cast<uint32_t>(m_Count); ++i)
            m_Free->Enqueue(i);   // all blocks initially free
    }

    // Returns a pointer to a free block and its index, or nullptr if exhausted.
    std::byte* Acquire(uint32_t& outIndex) {
        uint32_t idx;
        if (!m_Free->Dequeue(idx)) return nullptr;
        outIndex = idx;
        return Block(idx);
    }

    void Release(uint32_t index) { m_Free->Enqueue(index); }

    std::byte* Block(uint32_t index) { return m_Storage.data() + static_cast<size_t>(index) * m_BlockSize; }

    size_t BlockSize() const { return m_BlockSize; }
    size_t BlockCount() const { return m_Count; }

private:
    static size_t RoundUpPow2(size_t n) {
        size_t p = 1; while (p < n) p <<= 1; return p < 1 ? 1 : p;
    }
    // Free-index ring sized to the max supported block count. 4096 blocks is ample
    // for net payloads; a pool larger than this static cap is rejected at construction
    // by clamping (documented). Use a generous fixed capacity for the index ring.
    static constexpr size_t kMaxBlocks = 4096;
    using FreeRing = MpscRing<uint32_t, kMaxBlocks>;

    size_t                 m_BlockSize;
    size_t                 m_Count;
    std::vector<std::byte>  m_Storage;
    std::unique_ptr<FreeRing> m_Free;   // heap (large; avoid bloating the pool object)
};
```
> Note: `m_Count` is clamped to `kMaxBlocks` by construction contract — if `blockCount` rounds above `kMaxBlocks`, clamp it (add `if (m_Count > kMaxBlocks) m_Count = kMaxBlocks;` after the `RoundUpPow2`). Add `#include <memory>` for `unique_ptr`.

Fix the constructor to clamp:
```cpp
        : m_BlockSize(blockSize), m_Count(RoundUpPow2(blockCount)) {
        if (m_Count > kMaxBlocks) m_Count = kMaxBlocks;
        ...
```
Add `#include <memory>` to the includes.

- [ ] **Step 4: Run to verify it passes**

Build + run 3×. Expected: `All net tests passed.`

- [ ] **Step 5: Commit**

```bash
git add src/common/include/NetBufferPool.h tests/test_net.cpp
git commit -m "feat(net): lock-free fixed-block NetBufferPool"
```

---

## Task 3: Game-facing types + `NetServices` table

**Files:**
- Create: `src/common/include/NetServices.h`
- Modify: `src/common/CMakeLists.txt` (netlib include dir on `CommonHeaders` INTERFACE)
- Modify: `tests/test_net.cpp` (compile/value test)

- [ ] **Step 1: Write the failing test**

Add to `tests/test_net.cpp`:
```cpp
#include "NetServices.h"

static void test_net_types() {
    netlib::Endpoint ep{ "127.0.0.1", 9000 };
    NetServerConfig sc{}; sc.bind = ep;
    CHECK(sc.bind.port == 9000, "types: NetServerConfig.bind");
    CHECK(sc.gameResident == false, "types: gameResident defaults false");

    NetEvent ev{};
    ev.kind = NetEventKind::Message;
    ev.opcode = 7;
    CHECK(ev.kind == NetEventKind::Message && ev.opcode == 7, "types: NetEvent fields");
    CHECK(NetHandle::Invalid == NetHandle{0}, "types: NetHandle::Invalid is 0");
}
```
Add `test_net_types();` to `main`.

- [ ] **Step 2: Run to verify it fails**

Build → FAIL: `NetServices.h` not found.

- [ ] **Step 3: Write the header + CMake**

`src/common/include/NetServices.h`:
```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include <netlib/netlib.h>   // Endpoint, ConnConfig, ConnId, IIoServer/IIoClient, factories

// Engine-provided networking service table, threaded through SystemContext like
// NavServices. Engine populates it (NetServicesImpl::Init); GameThread threads a
// pointer each tick. All calls are GameThread-only (matches NavServices). Field
// order is append-only for Game.dll binary compat.

enum class NetHandle : uint32_t { Invalid = 0 };   // identifies a registered adapter

// Game-facing connection id (mirrors netlib::ConnId; servers fan out to many peers).
using NetConnId = uint64_t;
inline constexpr NetConnId kNetConnInvalid = 0;

enum class NetEventKind : uint8_t { Connected, Disconnected, Error, Message };

// One drained inbound event. `payload` (Message only) points into NetSubsystem's
// reusable drain buffer — valid until the next PollEvent. Copy it to retain.
struct NetEvent {
    NetEventKind   kind   = NetEventKind::Error;
    NetHandle      adapter = NetHandle::Invalid;   // which registered adapter
    NetConnId      conn   = kNetConnInvalid;       // which connection within it
    uint16_t       opcode = 0;                     // Message only
    const uint8_t* payload = nullptr;              // Message only; borrowed until next PollEvent
    uint32_t       len    = 0;
};

struct NetServerConfig {
    netlib::Endpoint   bind{};
    netlib::ConnConfig conn{};
    bool               gameResident = false;   // true => Game.dll-resident adapter (Model B teardown)
};
struct NetClientConfig {
    netlib::Endpoint   target{};
    netlib::ConnConfig conn{};
    bool               gameResident = false;
};

// Adapter factories the game supplies (e.g. &netlib::MakeTcpServer). Residence of
// the factory's DLL is irrelevant to the type; gameResident in the config declares intent.
using NetServerFactory = std::unique_ptr<netlib::IIoServer>(*)();
using NetClientFactory = std::unique_ptr<netlib::IIoClient>(*)();

struct NetServices {
    // Create a listening server / outbound client from a netlib factory. Engine owns
    // the adapter, wires its sink, and Starts it. Returns Invalid on failure.
    NetHandle (*CreateServer)(NetServerFactory factory, const NetServerConfig& cfg);
    NetHandle (*CreateClient)(NetClientFactory factory, const NetClientConfig& cfg);

    // Server's actually-bound port (resolves ephemeral port 0); 0 if not a server / not bound.
    uint16_t  (*BoundPort)(NetHandle h);

    // Send [uint16 opcode][data] to a connection. For a client adapter, pass
    // kNetConnInvalid (its single connection). Returns false on backpressure/unknown handle.
    bool      (*Send)(NetHandle h, NetConnId conn, uint16_t opcode, const uint8_t* data, size_t len);

    // Drain ONE event from the shared inbound MPSC ring (all adapters). False when empty.
    bool      (*PollEvent)(NetEvent* out);

    // Close + tear down one adapter (joins its threads via netlib Stop()).
    void      (*Close)(NetHandle h);
};
```

In `src/common/CMakeLists.txt`, add netlib's include dir to the `CommonHeaders` INTERFACE so every consumer of `Systems.h`/`NetServices.h` can compile. Current file:
```cmake
add_library(CommonHeaders INTERFACE)
target_include_directories(CommonHeaders INTERFACE include)
target_compile_definitions(CommonHeaders INTERFACE
    NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_directories(CommonHeaders INTERFACE glm::glm)
```
Change the include line to also expose netlib's headers:
```cmake
target_include_directories(CommonHeaders INTERFACE
    include
    ${CMAKE_SOURCE_DIR}/src/netlib/include)
```
> This makes `<netlib/...>` available wherever `CommonHeaders` is linked (engine, ecs, game, tests). It adds an include path only — no link dependency. netlib's public headers are header-only and Winsock-free, so this is safe and doesn't pull Winsock into ecs/game.

- [ ] **Step 4: Run to verify it passes**

Build + run. Expected: `All net tests passed.`

- [ ] **Step 5: Commit**

```bash
git add src/common/include/NetServices.h src/common/CMakeLists.txt tests/test_net.cpp
git commit -m "feat(net): NetServices bridge types (NetHandle/NetEvent/configs/table)"
```

---

## Task 4: `NetSubsystem` + `NetServicesImpl` + TCP-loopback proof

> **USER ACTION (manual, before first run of this task's test):** the round-trip test starts a real `netlib` IOCP server on `127.0.0.1` (calls `listen()`), so the **Windows Firewall "Allow" dialog may appear** — accept it. Loopback usually skips it; the test is timeout-guarded so an unaccepted prompt fails fast rather than hanging.

The engine-side singleton: the `IIoSink` adapter threads push into, owning the registry, per-adapter sinks, the MPSC ring, the pool, opcode framing, and the drain buffer.

**Files:**
- Create: `src/engine/src/network/NetSubsystem.h`
- Create: `src/engine/src/network/NetSubsystem.cpp`
- Create: `src/engine/src/network/NetServicesImpl.h`
- Create: `src/engine/src/network/NetServicesImpl.cpp`
- Modify: `src/engine/CMakeLists.txt` (sources + include dir + link netlib)
- Modify: `tests/test_net.cpp` (TCP-loopback round-trip + multi-message tests)

- [ ] **Step 1: Write the failing test**

Add to `tests/test_net.cpp` (include + helpers + test; call from `main`):
```cpp
#include "network/NetSubsystem.h"
#include "network/NetServicesImpl.h"
#include <chrono>

template <typename Pred>
static bool net_wait_until(Pred pred, int timeoutMs = 4000) {
    using clock = std::chrono::steady_clock;
    auto start = clock::now();
    while (!pred()) {
        if (std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - start).count() > timeoutMs)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

static std::vector<uint8_t> u8(const char* s) {
    std::vector<uint8_t> v; for (const char* p = s; *p; ++p) v.push_back((uint8_t)*p); return v;
}

static void test_netsub_roundtrip() {
    NetServices net{};
    NetServicesImpl::Init(net);
    NetSubsystem::Instance().Init();   // fresh subsystem (pool + ring)

    // Server (ephemeral port) + client over loopback, both real netlib TCP adapters.
    NetServerConfig sc{}; sc.bind = netlib::Endpoint{ "127.0.0.1", 0 };
    NetHandle srv = net.CreateServer(&netlib::MakeTcpServer, sc);
    CHECK(srv != NetHandle::Invalid, "netsub: server created");
    uint16_t port = net.BoundPort(srv);
    CHECK(port != 0, "netsub: server bound port");

    NetClientConfig cc{}; cc.target = netlib::Endpoint{ "127.0.0.1", port };
    NetHandle cli = net.CreateClient(&netlib::MakeTcpClient, cc);
    CHECK(cli != NetHandle::Invalid, "netsub: client created");

    // Drain events on THIS thread (the GameThread role). Collect until we see the
    // server-side Connected + the client's message.
    int serverConns = 0; bool gotClientMsg = false; NetConnId serverConn = kNetConnInvalid;
    auto pump = [&]{
        NetEvent ev{};
        while (net.PollEvent(&ev)) {
            if (ev.adapter == srv && ev.kind == NetEventKind::Connected) { ++serverConns; serverConn = ev.conn; }
            if (ev.adapter == srv && ev.kind == NetEventKind::Message) {
                if (ev.opcode == 42 && ev.len == 5 && std::memcmp(ev.payload, "hello", 5) == 0) gotClientMsg = true;
            }
        }
    };
    CHECK(net_wait_until([&]{ pump(); return serverConns == 1; }), "netsub: server saw a connection");

    auto msg = u8("hello");
    CHECK(net.Send(cli, kNetConnInvalid, /*opcode*/42, msg.data(), msg.size()), "netsub: client send");
    CHECK(net_wait_until([&]{ pump(); return gotClientMsg; }), "netsub: server received opcode+payload");

    // Server replies to that connection; client receives it.
    bool gotReply = false;
    auto reply = u8("yo");
    net.Send(srv, serverConn, /*opcode*/7, reply.data(), reply.size());
    CHECK(net_wait_until([&]{
        NetEvent ev{};
        while (net.PollEvent(&ev)) if (ev.adapter == cli && ev.kind == NetEventKind::Message
                                        && ev.opcode == 7 && ev.len == 2 && std::memcmp(ev.payload, "yo", 2) == 0) gotReply = true;
        return gotReply;
    }), "netsub: client received server reply");

    net.Close(cli);
    net.Close(srv);
    NetSubsystem::Instance().Shutdown();
}
```
Add `test_netsub_roundtrip();` to `main`.

- [ ] **Step 2: Run to verify it fails**

Build → FAIL: `network/NetSubsystem.h` not found.

- [ ] **Step 3: Implement NetSubsystem + NetServicesImpl + CMake**

`src/engine/src/network/NetSubsystem.h`:
```cpp
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "Engine.h"
#include "MpscRing.h"
#include "NetBufferPool.h"
#include "NetServices.h"
#include <netlib/netlib.h>

// Engine-side networking singleton (GameThread-only API; netlib adapter threads call
// the per-adapter sinks). Owns the registry, the inbound MPSC ring, the payload pool.
// NOTE (Phase 4): singleton matches NavMeshSystem; in-process dual-world will require
// de-singletoning (see umbrella spec section 9).
class ENGINE_API NetSubsystem {
public:
    static NetSubsystem& Instance();

    void Init();        // (re)initialize pool + ring; idempotent-safe for tests
    void Shutdown();    // close all adapters (joins their threads), clear state

    NetHandle CreateServer(NetServerFactory factory, const NetServerConfig& cfg);
    NetHandle CreateClient(NetClientFactory factory, const NetClientConfig& cfg);
    uint16_t  BoundPort(NetHandle h);
    bool      Send(NetHandle h, NetConnId conn, uint16_t opcode, const uint8_t* data, size_t len);
    bool      PollEvent(NetEvent* out);
    void      Close(NetHandle h);

    // Model-B: tear down adapters created with gameResident=true (joins their threads)
    // before Game.dll unload. Called from GameThread before LoadOrReload.
    void      ReleaseGameResidentConnections();

private:
    NetSubsystem() = default;

    // Per-adapter sink: tags netlib IoEvents with the owning NetHandle and forwards
    // to OnAdapterEvent. Lives in the registry Entry; called from adapter threads.
    struct AdapterSink final : netlib::IIoSink {
        NetSubsystem* self = nullptr;
        NetHandle     handle = NetHandle::Invalid;
        void OnIoEvent(const netlib::IoEvent& ev) override { self->OnAdapterEvent(handle, ev); }
    };

    struct Entry {
        std::unique_ptr<netlib::IIoServer> server;   // one of server/client is set
        std::unique_ptr<netlib::IIoClient> client;
        std::unique_ptr<AdapterSink>       sink;
        bool gameResident = false;
    };

    // Ring element (POD). For Message, payload bytes live in pool block `poolIndex`.
    struct RingEvent {
        NetEventKind kind;
        NetHandle    adapter;
        NetConnId    conn;
        uint16_t     opcode;
        uint32_t     poolIndex;   // valid only for Message
        uint32_t     len;
        bool         hasPayload;
    };

    void OnAdapterEvent(NetHandle h, const netlib::IoEvent& ev);   // adapter-thread side

    static constexpr size_t kRingSize  = 4096;     // power of two
    static constexpr size_t kBlockSize = 64 * 1024;
    static constexpr size_t kBlocks    = 1024;

    std::mutex m_Mx;                                // guards the registry only (not the hot path)
    std::unordered_map<uint32_t, Entry> m_Adapters;
    uint32_t m_NextHandle = 1;                      // 0 = Invalid

    std::unique_ptr<MpscRing<RingEvent, kRingSize>> m_Ring;
    std::unique_ptr<NetBufferPool>                  m_Pool;
    std::vector<uint8_t>                            m_DrainBuf;   // GameThread-only reuse buffer
};
```

`src/engine/src/network/NetSubsystem.cpp`:
```cpp
#include "network/NetSubsystem.h"

#include <cstring>

#include "lib.h"

namespace {
constexpr uint32_t kOpcodeBytes = 2;   // [uint16 LE opcode] prefix inside each netlib frame
}

NetSubsystem& NetSubsystem::Instance() {
    static NetSubsystem s;
    return s;
}

void NetSubsystem::Init() {
    Shutdown();   // clean slate
    m_Ring = std::make_unique<MpscRing<RingEvent, kRingSize>>();
    m_Pool = std::make_unique<NetBufferPool>(kBlockSize, kBlocks);
    m_DrainBuf.resize(kBlockSize);
}

void NetSubsystem::Shutdown() {
    // Stop all adapters first (joins their threads) so no sink fires during teardown.
    {
        std::scoped_lock lk(m_Mx);
        for (auto& [id, e] : m_Adapters) {
            if (e.server) e.server->Stop();
            if (e.client) e.client->Stop();
        }
        m_Adapters.clear();
    }
    m_Ring.reset();
    m_Pool.reset();
}

NetHandle NetSubsystem::CreateServer(NetServerFactory factory, const NetServerConfig& cfg) {
    if (!factory || !m_Ring) return NetHandle::Invalid;
    auto server = factory();
    if (!server) return NetHandle::Invalid;

    uint32_t id;
    AdapterSink* sinkPtr = nullptr;
    netlib::IIoServer* srv = server.get();   // raw ptr stays valid after the unique_ptr is moved into the map
    {
        std::scoped_lock lk(m_Mx);
        id = m_NextHandle++;
        Entry e;
        e.gameResident = cfg.gameResident;
        e.sink = std::make_unique<AdapterSink>();
        e.sink->self = this;
        e.sink->handle = NetHandle{ id };
        sinkPtr = e.sink.get();              // AdapterSink pointee survives the Entry move
        e.server = std::move(server);
        m_Adapters.emplace(id, std::move(e));
    }
    // Start outside the lock (spawns the accept thread + IOCP workers).
    if (!srv->Start(cfg.bind, cfg.conn, sinkPtr)) {
        SM_WARN("NetSubsystem: server Start failed");
        Close(NetHandle{ id });
        return NetHandle::Invalid;
    }
    return NetHandle{ id };
}

NetHandle NetSubsystem::CreateClient(NetClientFactory factory, const NetClientConfig& cfg) {
    if (!factory || !m_Ring) return NetHandle::Invalid;
    auto client = factory();
    if (!client) return NetHandle::Invalid;

    uint32_t id;
    AdapterSink* sinkPtr = nullptr;
    netlib::IIoClient* cli = client.get();   // raw ptr stays valid after the unique_ptr is moved into the map
    {
        std::scoped_lock lk(m_Mx);
        id = m_NextHandle++;
        Entry e;
        e.gameResident = cfg.gameResident;
        e.sink = std::make_unique<AdapterSink>();
        e.sink->self = this;
        e.sink->handle = NetHandle{ id };
        sinkPtr = e.sink.get();              // AdapterSink pointee survives the Entry move
        e.client = std::move(client);
        m_Adapters.emplace(id, std::move(e));
    }
    // Start outside the lock (spawns the connect thread).
    if (!cli->Start(cfg.target, cfg.conn, sinkPtr)) {
        SM_WARN("NetSubsystem: client Start failed");
        Close(NetHandle{ id });
        return NetHandle::Invalid;
    }
    return NetHandle{ id };
}

uint16_t NetSubsystem::BoundPort(NetHandle h) {
    std::scoped_lock lk(m_Mx);
    auto it = m_Adapters.find(static_cast<uint32_t>(h));
    if (it == m_Adapters.end() || !it->second.server) return 0;
    return it->second.server->BoundPort();
}

bool NetSubsystem::Send(NetHandle h, NetConnId conn, uint16_t opcode, const uint8_t* data, size_t len) {
    // Build [uint16 LE opcode][payload] and hand to the adapter (it length-frames it).
    std::vector<std::byte> buf;
    buf.resize(kOpcodeBytes + len);
    buf[0] = static_cast<std::byte>(opcode & 0xff);
    buf[1] = static_cast<std::byte>((opcode >> 8) & 0xff);
    if (len) std::memcpy(buf.data() + kOpcodeBytes, data, len);

    std::scoped_lock lk(m_Mx);
    auto it = m_Adapters.find(static_cast<uint32_t>(h));
    if (it == m_Adapters.end()) return false;
    if (it->second.server) { it->second.server->Send(netlib::ConnId{ conn }, buf); return true; }
    if (it->second.client) { it->second.client->Send(buf); return true; }
    return false;
}

void NetSubsystem::OnAdapterEvent(NetHandle h, const netlib::IoEvent& ev) {
    // Runs on adapter threads (IOCP workers / client IO thread). Lock-free hot path:
    // pool acquire + ring enqueue only. Never touches the registry.
    if (!m_Ring || !m_Pool) return;
    RingEvent re{};
    re.adapter = h;
    re.conn    = static_cast<NetConnId>(ev.conn);
    re.hasPayload = false;

    switch (ev.kind) {
        case netlib::IoEvent::Kind::Connected:    re.kind = NetEventKind::Connected; break;
        case netlib::IoEvent::Kind::Disconnected: re.kind = NetEventKind::Disconnected; break;
        case netlib::IoEvent::Kind::Error:        re.kind = NetEventKind::Error; break;
        case netlib::IoEvent::Kind::Message: {
            re.kind = NetEventKind::Message;
            const auto& p = ev.payload;
            if (p.size() < kOpcodeBytes) { SM_WARN("NetSubsystem: runt frame (%zu bytes); dropped", p.size()); return; }
            re.opcode = static_cast<uint16_t>(static_cast<uint8_t>(p[0])) |
                        (static_cast<uint16_t>(static_cast<uint8_t>(p[1])) << 8);
            const size_t payloadLen = p.size() - kOpcodeBytes;
            if (payloadLen > m_Pool->BlockSize()) { SM_WARN("NetSubsystem: payload %zu > block %zu; dropped", payloadLen, m_Pool->BlockSize()); return; }
            uint32_t idx;
            std::byte* block = m_Pool->Acquire(idx);
            if (!block) { SM_WARN("NetSubsystem: pool exhausted; message dropped"); return; }
            if (payloadLen) std::memcpy(block, p.data() + kOpcodeBytes, payloadLen);
            re.poolIndex = idx;
            re.len = static_cast<uint32_t>(payloadLen);
            re.hasPayload = true;
            break;
        }
    }

    if (!m_Ring->Enqueue(re)) {
        SM_WARN("NetSubsystem: inbound ring full; event dropped");
        if (re.hasPayload) m_Pool->Release(re.poolIndex);   // don't leak the block
    }
}

bool NetSubsystem::PollEvent(NetEvent* out) {
    if (!out || !m_Ring) return false;
    RingEvent re{};
    if (!m_Ring->Dequeue(re)) return false;
    out->kind    = re.kind;
    out->adapter = re.adapter;
    out->conn    = re.conn;
    out->opcode  = re.opcode;
    out->payload = nullptr;
    out->len     = 0;
    if (re.hasPayload) {
        // Copy into the reusable drain buffer (GameThread is the sole consumer),
        // release the pool block, point payload at the drain buffer.
        if (m_DrainBuf.size() < re.len) m_DrainBuf.resize(re.len);
        std::memcpy(m_DrainBuf.data(), m_Pool->Block(re.poolIndex), re.len);
        m_Pool->Release(re.poolIndex);
        out->payload = m_DrainBuf.data();
        out->len     = re.len;
    }
    return true;
}

void NetSubsystem::Close(NetHandle h) {
    std::unique_ptr<netlib::IIoServer> srv;
    std::unique_ptr<netlib::IIoClient> cli;
    std::unique_ptr<AdapterSink> sink;
    {
        std::scoped_lock lk(m_Mx);
        auto it = m_Adapters.find(static_cast<uint32_t>(h));
        if (it == m_Adapters.end()) return;
        srv  = std::move(it->second.server);
        cli  = std::move(it->second.client);
        sink = std::move(it->second.sink);
        m_Adapters.erase(it);
    }
    // Stop OUTSIDE the lock — Stop() joins adapter threads; the sink (still alive in
    // `sink`) may be mid-OnIoEvent, but it only touches the (always-valid) ring+pool.
    if (srv) srv->Stop();
    if (cli) cli->Stop();
    // srv/cli/sink destruct here, after threads joined.
}

void NetSubsystem::ReleaseGameResidentConnections() {
    std::vector<NetHandle> toClose;
    {
        std::scoped_lock lk(m_Mx);
        for (auto& [id, e] : m_Adapters) if (e.gameResident) toClose.push_back(NetHandle{ id });
    }
    for (NetHandle h : toClose) Close(h);   // Close() joins threads (the quiesce)
}
```
> **Implementer note:** capture the raw adapter pointer (`server.get()`/`client.get()`) and the sink pointer (`e.sink.get()`) BEFORE moving the `unique_ptr`s into the map — the pointees keep their heap addresses across the move, so these raw pointers stay valid. Call `Start()` OUTSIDE the `m_Mx` lock (it spawns threads; holding the registry lock across thread creation would needlessly serialize and risk lock-ordering issues with the sink path).

`src/engine/src/network/NetServicesImpl.h`:
```cpp
#pragma once

#include "Engine.h"
#include "NetServices.h"

namespace NetServicesImpl {
    // Populate the fn-ptr table (forwards to NetSubsystem::Instance()). Idempotent.
    ENGINE_API void Init(NetServices& out);
}
```

`src/engine/src/network/NetServicesImpl.cpp`:
```cpp
#include "network/NetServicesImpl.h"
#include "network/NetSubsystem.h"

namespace {
NetHandle FwdCreateServer(NetServerFactory f, const NetServerConfig& c) { return NetSubsystem::Instance().CreateServer(f, c); }
NetHandle FwdCreateClient(NetClientFactory f, const NetClientConfig& c) { return NetSubsystem::Instance().CreateClient(f, c); }
uint16_t  FwdBoundPort(NetHandle h) { return NetSubsystem::Instance().BoundPort(h); }
bool      FwdSend(NetHandle h, NetConnId conn, uint16_t op, const uint8_t* d, size_t n) { return NetSubsystem::Instance().Send(h, conn, op, d, n); }
bool      FwdPollEvent(NetEvent* o) { return NetSubsystem::Instance().PollEvent(o); }
void      FwdClose(NetHandle h) { NetSubsystem::Instance().Close(h); }
}

namespace NetServicesImpl {
void Init(NetServices& out) {
    out.CreateServer = &FwdCreateServer;
    out.CreateClient = &FwdCreateClient;
    out.BoundPort    = &FwdBoundPort;
    out.Send         = &FwdSend;
    out.PollEvent    = &FwdPollEvent;
    out.Close        = &FwdClose;
}
}
```

In `src/engine/CMakeLists.txt`:
- Add to the source list (after the navigation block, line ~64):
  ```cmake
      # Networking (netlib integration)
      src/network/NetSubsystem.cpp
      src/network/NetServicesImpl.cpp
  ```
- Add `src/network` to `target_include_directories(Engine PUBLIC ...)` (after `src/navigation`).
- Add `netlib` to `target_link_libraries(Engine PUBLIC ...)` (after `ecs`).

- [ ] **Step 4: Run to verify it passes**

Build `test_net`, run (accept firewall prompt if shown), 3×. Expected: `All net tests passed.`

- [ ] **Step 5: Commit**

```bash
git add src/engine/src/network tests/test_net.cpp src/engine/CMakeLists.txt
git commit -m "feat(net): NetSubsystem + NetServices bridge (TCP-loopback round-trip via engine)"
```

---

## Task 5: SystemContext wiring + Model-B teardown + manual smoke

Thread `NetServices` through `SystemContext` each tick and tear down game-resident adapters before `Game.dll` reload. Engine code → requires editor restart; the automated check is the teardown unit test; live behavior is a manual smoke.

**Files:**
- Modify: `src/common/include/Systems.h` (add `Net` field)
- Modify: `src/engine/src/threading/GameThread.cpp` (init + thread + teardown)
- Modify: `tests/test_net.cpp` (teardown test)

- [ ] **Step 1: Write the failing test**

Add to `tests/test_net.cpp` (call from `main`):
```cpp
static void test_release_game_resident() {
    NetServices net{}; NetServicesImpl::Init(net);
    NetSubsystem::Instance().Init();

    // A game-resident server + a non-resident server.
    NetServerConfig resident{};   resident.bind = netlib::Endpoint{ "127.0.0.1", 0 }; resident.gameResident = true;
    NetServerConfig stable{};     stable.bind   = netlib::Endpoint{ "127.0.0.1", 0 }; stable.gameResident = false;
    NetHandle hr = net.CreateServer(&netlib::MakeTcpServer, resident);
    NetHandle hs = net.CreateServer(&netlib::MakeTcpServer, stable);
    CHECK(hr != NetHandle::Invalid && hs != NetHandle::Invalid, "teardown: both created");

    NetSubsystem::Instance().ReleaseGameResidentConnections();

    // Resident one is gone (BoundPort 0 = unknown handle); stable one still bound.
    CHECK(net.BoundPort(hr) == 0, "teardown: game-resident adapter released");
    CHECK(net.BoundPort(hs) != 0, "teardown: stable adapter survives");

    NetSubsystem::Instance().Shutdown();
}
```
Add `test_release_game_resident();` to `main`.

- [ ] **Step 2: Run to verify it fails**

It will fail to compile only if something's missing; `ReleaseGameResidentConnections` exists from Task 4, so this test should actually **pass already** against Task 4's code. Run it: if it passes, good (it locks in the contract). The real subject of Task 5 is the GameThread/SystemContext wiring (compile-gated). Proceed to wire.

- [ ] **Step 3: Add `Net` to `SystemContext`**

In `src/common/include/Systems.h`, add the include and field:
```cpp
#include "NetServices.h"   // add near the top, after NavServices.h include
```
and in `struct SystemContext`, after the `Nav` line:
```cpp
    const NetServices* Net = nullptr;  // engine-provided net table; nullptr in tests/pre-init
```

- [ ] **Step 4: Wire GameThread**

In `src/engine/src/threading/GameThread.cpp`:
- Add includes near the navigation includes (top of file):
  ```cpp
  #include "network/NetServicesImpl.h"
  #include "network/NetSubsystem.h"
  ```
- Near the `NavServices navServices{}; NavServicesImpl::Init(navServices);` block (around line 163), add:
  ```cpp
  // NetServices function-pointer table — engine networking surface for game systems.
  NetServices netServices{};
  NetServicesImpl::Init(netServices);
  NetSubsystem::Instance().Init();
  ```
- In the reload-drain block (the `if (m_ReloadPending.exchange(false, ...))` at ~line 194), BEFORE `m_GameLib.LoadOrReload(...)`, add the Model-B teardown:
  ```cpp
  // Model B: release Game.dll-resident network adapters (joins their threads) before
  // the DLL is unloaded — the networking analog of SystemScheduler::Clear().
  NetSubsystem::Instance().ReleaseGameResidentConnections();
  ```
- In the `SystemContext sysCtx{ gameState.World, gameState.DeltaTime, gameState.GameTime, &navServices };` line (~447), add `&netServices`:
  ```cpp
  SystemContext sysCtx{ gameState.World, gameState.DeltaTime, gameState.GameTime, &navServices, &netServices };
  ```
- At the end of `RunLoop`, after `m_GameLib.Unload(&gameState);` / before returning, add a clean shutdown:
  ```cpp
  NetSubsystem::Instance().Shutdown();
  ```

- [ ] **Step 5: Build everything + run test_net**

Run:
```
cmake --build --preset msvc-win64-vs2026-community --target test_net
cmake --build --preset msvc-win64-vs2026-community --target editor
```
Both must compile. Run `test_net.exe` → `All net tests passed.` (3×).

- [ ] **Step 6: Manual smoke (USER-involved, not a blocking gate)**

Engine changes need an editor restart. The umbrella proof ("a game NetSystem round-trips + survives hot-reload") needs a real game system, which is Phase-3+ territory. For Phase 2, the automated `test_net` round-trip + teardown tests are the gate. Document in the commit body that live in-editor verification is deferred to when a game NetSystem exists (Phase 3). Confirm the editor at least **launches without crashing** with the wiring in place (`NetSubsystem::Instance().Init()` on GameThread start, Shutdown on exit).

- [ ] **Step 7: Commit**

```bash
git add src/common/include/Systems.h src/engine/src/threading/GameThread.cpp tests/test_net.cpp
git commit -m "feat(net): thread NetServices through SystemContext + Model-B reload teardown"
```

---

## Final verification

- [ ] `test_net` passes all sections 3× (lock-free ring + pool stress, NetSubsystem TCP round-trip, teardown).
- [ ] `editor` + `runtime` still build (the `SystemContext` field addition + GameThread wiring compile across all consumers).
- [ ] `Game.dll` still builds and the game target still links (it now transitively sees `NetServices.h` via `Systems.h`; confirm it compiles — `Game.dll` does NOT need to link `netlib` unless game code calls a netlib factory, which it doesn't yet).
- [ ] Per subagent-driven-development: final code review of `NetSubsystem` (registry locking vs the lock-free hot path; pool block never leaked on ring-full; Close/Shutdown join ordering; OnAdapterEvent thread-safety) + the lock-free `MpscRing`/`NetBufferPool`.

---

## Notes for the executor

- **`MpscRing` + `NetBufferPool` are the risk centre** (lock-free). Stress-test hard (the concurrent tests run many iterations). The Vyukov algorithm is standard — reproduce it exactly; don't improvise memory orderings.
- **Hot path is lock-free; registry is locked.** `OnAdapterEvent` (adapter threads) only touches the ring + pool (lock-free); the `m_Mx` mutex guards only create/close/registry — never the per-message path. Don't accidentally take `m_Mx` in `OnAdapterEvent`/`PollEvent`.
- **Pool block leak on ring-full:** `OnAdapterEvent` must `Release` the block if `Enqueue` fails (it does). Verify.
- **Close/Shutdown join ordering:** `Close` moves the adapter out of the registry under the lock, then `Stop()`s it OUTSIDE the lock (Stop joins threads). The sink object stays alive (moved into a local) until after the join, so an in-flight `OnIoEvent` is safe (it only touches ring+pool).
- **Firewall/UAC** is a user-owned manual gate for the Task-4 TCP round-trip test (first `listen()`). Tests time out rather than hang.
- **`Game.dll` link graph:** including `NetServices.h` (via `Systems.h`) only adds netlib *headers* (header-only, Winsock-free). The game links netlib only when it calls a factory — defer that to Phase 3.
```
