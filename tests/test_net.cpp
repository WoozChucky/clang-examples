#include <cstdio>
#include <atomic>
#include <thread>
#include <vector>

#include "MpscRing.h"
#include "NetBufferPool.h"
#include "NetServices.h"
#include "network/NetSubsystem.h"
#include "network/NetServicesImpl.h"
#include <chrono>
#include <cstring>
#include <utility>   // std::move

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
    constexpr int K = 8, M = 10000;
    MpscRing<int, 1024> r;
    std::atomic<int> producedTotal{0};
    std::vector<std::thread> producers;
    std::atomic<bool> go{false};
    for (int k = 0; k < K; ++k) {
        producers.emplace_back([&]{
            while (!go.load()) {}
            for (int i = 0; i < M; ++i) {
                while (!r.Enqueue(1)) { std::this_thread::yield(); }
                producedTotal.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    go.store(true);
    int received = 0;
    int out = 0;
    while (received < K * M) {
        if (r.Dequeue(out)) received += out;
        else std::this_thread::yield();
    }
    for (auto& t : producers) t.join();
    CHECK(received == K * M, "mpsc: concurrent received exactly K*M");
    CHECK(!r.Dequeue(out), "mpsc: empty after draining all");
}

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

// Peak must be a TRUE high-water mark recorded at Acquire, so it survives Release.
// Sampling InUse at read time misses short-lived allocations (e.g. send buffers
// released on the IOCP worker microseconds after acquire) — that's the send-pool
// "peak 0" bug. PeakInUse must latch the max regardless of when it's read.
static void test_pool_peak() {
    NetBufferPool pool(64, 4);
    CHECK(pool.PeakInUse() == 0, "pool: peak starts 0");
    uint32_t a, b;
    pool.Acquire(a);
    pool.Acquire(b);
    CHECK(pool.InUse() == 2, "pool: 2 in use");
    CHECK(pool.PeakInUse() == 2, "pool: peak 2 while held");
    pool.Release(a);
    pool.Release(b);
    CHECK(pool.InUse() == 0, "pool: 0 in use after release");
    CHECK(pool.PeakInUse() == 2, "pool: peak high-water survives release");
}

#include <set>
// Ring-direct MPMC probe (isolates MpscRing from NetBufferPool): seed 0..255, then
// 8 threads each Dequeue+Enqueue heavily; afterward exactly 0..255 must remain — no
// loss, no duplication.
static void test_mpsc_mpmc() {
    auto ring = std::make_unique<MpscRing<uint32_t, 4096>>();
    for (uint32_t i = 0; i < 256; ++i) ring->Enqueue(i);
    constexpr int K = 8, iters = 20000;
    std::vector<std::thread> ts; std::atomic<bool> go{false};
    for (int k = 0; k < K; ++k) ts.emplace_back([&]{
        while (!go.load()) {}
        // Recycle pattern: a free-list caller must RETRY Enqueue (transient-full under
        // MPMC when a consumer is preempted mid-dequeue), never drop — mirrors NetBufferPool::Release.
        for (int i = 0; i < iters; ++i) {
            uint32_t v;
            if (ring->Dequeue(v)) { while (!ring->Enqueue(v)) std::this_thread::yield(); }
        }
    });
    go.store(true);
    for (auto& t : ts) t.join();
    std::multiset<uint32_t> got; uint32_t v;
    while (ring->Dequeue(v)) got.insert(v);
    bool distinctOk = (got.size() == 256);
    for (uint32_t i = 0; i < 256 && distinctOk; ++i) if (got.count(i) != 1) distinctOk = false;
    if (!distinctOk) std::printf("  mpmc DIAG: remaining=%zu (expected 256)\n", got.size());
    CHECK(distinctOk, "mpsc: MPMC churn with retry preserves all 256 indices (no loss/dup)");
}

// PRODUCTION pattern for the inbound NetEvent ring: MANY producers, ONE consumer.
// K producer threads each enqueue M distinct-tagged items; one consumer drains
// exactly K*M with no loss/dup. (If this fails, the inbound ring itself is broken.)
static void test_mpsc_single_consumer() {
    constexpr int K = 8, M = 50000;
    auto ring = std::make_unique<MpscRing<uint64_t, 1024>>();
    std::vector<std::thread> producers; std::atomic<bool> go{false};
    for (int k = 0; k < K; ++k) producers.emplace_back([&, k]{
        while (!go.load()) {}
        for (int i = 0; i < M; ++i) {
            const uint64_t tag = (static_cast<uint64_t>(k) << 32) | static_cast<uint32_t>(i);
            while (!ring->Enqueue(tag)) { /* full: spin (consumer drains) */ }
        }
    });
    go.store(true);
    std::set<uint64_t> seen; int received = 0; bool dup = false; uint64_t v;
    while (received < K * M) {
        if (ring->Dequeue(v)) { if (!seen.insert(v).second) dup = true; ++received; }
    }
    for (auto& t : producers) t.join();
    CHECK(received == K * M && !dup, "mpsc: single-consumer received exactly K*M, no dup (inbound-ring pattern)");
    CHECK(!ring->Dequeue(v), "mpsc: single-consumer empty after drain");
}

static void test_pool_concurrent() {
    NetBufferPool pool(64, 256);
    constexpr int K = 8, iters = 20000;
    std::vector<std::thread> ts;
    std::atomic<bool> go{false};
    for (int k = 0; k < K; ++k) ts.emplace_back([&]{
        while (!go.load()) {}
        for (int i = 0; i < iters; ++i) {
            uint32_t idx;
            std::byte* p = pool.Acquire(idx);
            if (p) { p[0] = std::byte{1}; pool.Release(idx); }
        }
    });
    go.store(true);
    for (auto& t : ts) t.join();
    std::vector<uint32_t> held;
    for (int i = 0; i < 256; ++i) { uint32_t idx; if (pool.Acquire(idx)) held.push_back(idx); }
    CHECK(held.size() == 256, "pool: all blocks free after concurrent churn (no leak/double-free)");
    for (uint32_t idx : held) pool.Release(idx);
    if (held.size() != 256) std::printf("  pool DIAG: reacquired=%zu (expected 256)\n", held.size());
}

static void test_net_types() {
    netlib::Endpoint ep{ "127.0.0.1", 9000 };
    NetServerConfig sc{}; sc.bind = ep;
    CHECK(sc.bind.port == 9000, "types: NetServerConfig.bind");
    CHECK(sc.gameResident == false, "types: gameResident defaults false");

    NetEvent ev{};
    ev.kind = NetEventKind::Message;
    ev.len  = 5;
    CHECK(ev.kind == NetEventKind::Message && ev.len == 5, "types: NetEvent fields");
    CHECK(NetHandle::Invalid == NetHandle{0}, "types: NetHandle::Invalid is 0");
}

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

// Send helper: writes a [u16 tag] + body into an acquired SendBuffer, then Sends.
static bool send_tagged(NetServices& net, NetHandle h, NetConnId conn, uint16_t tag, const std::vector<uint8_t>& body) {
    SendBuffer sb = net.AcquireSend(2 + body.size());
    if (!sb.data) return false;
    sb.data[0] = static_cast<uint8_t>(tag & 0xff);
    sb.data[1] = static_cast<uint8_t>((tag >> 8) & 0xff);
    std::memcpy(sb.data + 2, body.data(), body.size());
    return net.Send(h, conn, std::move(sb), static_cast<uint32_t>(2 + body.size()));
}
// Read the [u16 tag] from a received frame.
static uint16_t frame_tag(const NetEvent& ev) {
    return static_cast<uint16_t>(ev.payload[0]) | (static_cast<uint16_t>(ev.payload[1]) << 8);
}

static void test_netsub_roundtrip() {
    NetServices net{};
    NetServicesImpl::Init(net);
    NetSubsystem::Instance().Init();

    NetServerConfig sc{}; sc.bind = netlib::Endpoint{ "127.0.0.1", 0 };
    NetHandle srv = net.CreateServer(&netlib::MakeTcpServer, sc);
    CHECK(srv != NetHandle::Invalid, "netsub: server created");
    uint16_t port = net.BoundPort(srv);
    CHECK(port != 0, "netsub: server bound port");

    NetClientConfig cc{}; cc.target = netlib::Endpoint{ "127.0.0.1", port };
    NetHandle cli = net.CreateClient(&netlib::MakeTcpClient, cc);
    CHECK(cli != NetHandle::Invalid, "netsub: client created");

    int serverConns = 0; bool gotClientMsg = false; NetConnId serverConn = kNetConnInvalid;
    auto pump = [&]{
        NetEvent ev{};
        while (net.PollEvent(&ev)) {
            if (ev.adapter == srv && ev.kind == NetEventKind::Connected) { ++serverConns; serverConn = ev.conn; }
            if (ev.adapter == srv && ev.kind == NetEventKind::Message) {
                if (ev.len == 7 && frame_tag(ev) == 42 && std::memcmp(ev.payload + 2, "hello", 5) == 0) gotClientMsg = true;
            }
        }
    };
    CHECK(net_wait_until([&]{ pump(); return serverConns == 1; }), "netsub: server saw a connection");

    CHECK(send_tagged(net, cli, kNetConnInvalid, 42, u8("hello")), "netsub: client send");
    CHECK(net_wait_until([&]{ pump(); return gotClientMsg; }), "netsub: server received tag+payload");

    bool gotReply = false;
    send_tagged(net, srv, serverConn, 7, u8("yo"));
    CHECK(net_wait_until([&]{
        NetEvent ev{};
        while (net.PollEvent(&ev)) if (ev.adapter == cli && ev.kind == NetEventKind::Message
                                        && ev.len == 4 && frame_tag(ev) == 7 && std::memcmp(ev.payload + 2, "yo", 2) == 0) gotReply = true;
        return gotReply;
    }), "netsub: client received server reply");

    net.Close(cli);
    net.Close(srv);
    NetSubsystem::Instance().Shutdown();
}

static void test_release_game_resident() {
    NetServices net{}; NetServicesImpl::Init(net);
    NetSubsystem::Instance().Init();

    NetServerConfig resident{}; resident.bind = netlib::Endpoint{ "127.0.0.1", 0 }; resident.gameResident = true;
    NetServerConfig stable{};   stable.bind   = netlib::Endpoint{ "127.0.0.1", 0 }; stable.gameResident = false;
    NetHandle hr = net.CreateServer(&netlib::MakeTcpServer, resident);
    NetHandle hs = net.CreateServer(&netlib::MakeTcpServer, stable);
    CHECK(hr != NetHandle::Invalid && hs != NetHandle::Invalid, "teardown: both created");

    NetSubsystem::Instance().ReleaseGameResidentConnections();

    CHECK(net.BoundPort(hr) == 0, "teardown: game-resident adapter released");
    CHECK(net.BoundPort(hs) != 0, "teardown: stable adapter survives");

    NetSubsystem::Instance().Shutdown();
}

// Mimics the application-close path: a connected server+client pair torn down by
// a single Shutdown() WITHOUT individually Close()-ing first (the round-trip test
// Closes each before Shutdown; the editor/runtime does NOT — it goes straight to
// NetSubsystem::Shutdown with both adapters live + connected to each other).
static void test_netsub_shutdown_while_connected() {
    NetServices net{}; NetServicesImpl::Init(net);
    NetSubsystem::Instance().Init();

    NetServerConfig sc{}; sc.bind = netlib::Endpoint{ "127.0.0.1", 0 }; sc.gameResident = true;
    NetHandle srv = net.CreateServer(&netlib::MakeTcpServer, sc);
    CHECK(srv != NetHandle::Invalid, "shutdown-live: server created");
    uint16_t port = net.BoundPort(srv);
    NetClientConfig cc{}; cc.target = netlib::Endpoint{ "127.0.0.1", port }; cc.gameResident = true;
    NetHandle cli = net.CreateClient(&netlib::MakeTcpClient, cc);
    CHECK(cli != NetHandle::Invalid, "shutdown-live: client created");

    int conns = 0;
    net_wait_until([&]{ NetEvent ev{}; while (net.PollEvent(&ev)) if (ev.kind == NetEventKind::Connected) ++conns; return conns >= 2; });
    auto m = u8("hi");
    send_tagged(net, cli, kNetConnInvalid, 1, m);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Tear down with BOTH adapters still live + connected — the app-close path.
    NetSubsystem::Instance().Shutdown();
    CHECK(true, "shutdown-live: Shutdown() of a connected pair did not crash/hang");
}

// Faithful repro of the editor's NetDemoSystem at close: continuous BIDIRECTIONAL
// traffic (client pings, server echoes) so both adapters have in-flight ops, then
// straight to Shutdown() WITHOUT draining (the app destroys the system then Shutdowns).
static void test_netsub_traffic_then_shutdown() {
    NetServices net{}; NetServicesImpl::Init(net);
    NetSubsystem::Instance().Init();

    NetServerConfig sc{}; sc.bind = netlib::Endpoint{ "127.0.0.1", 0 }; sc.gameResident = true;
    NetHandle srv = net.CreateServer(&netlib::MakeTcpServer, sc);
    uint16_t port = net.BoundPort(srv);
    NetClientConfig cc{}; cc.target = netlib::Endpoint{ "127.0.0.1", port }; cc.gameResident = true;
    NetHandle cli = net.CreateClient(&netlib::MakeTcpClient, cc);
    CHECK(srv != NetHandle::Invalid && cli != NetHandle::Invalid, "traffic: created");

    using clock = std::chrono::steady_clock;
    auto start = clock::now();
    auto m = u8("ping");
    while (std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - start).count() < 400) {
        NetEvent ev{};
        while (net.PollEvent(&ev)) {
            if (ev.adapter == srv && ev.kind == NetEventKind::Message) {
                std::vector<uint8_t> body(ev.payload, ev.payload + ev.len);   // copy before next PollEvent
                SendBuffer sb = net.AcquireSend(body.size());
                if (sb.data) { std::memcpy(sb.data, body.data(), body.size()); net.Send(srv, ev.conn, std::move(sb), (uint32_t)body.size()); }
            }
        }
        send_tagged(net, cli, kNetConnInvalid, 1, m);   // client pings hard (no 2s gap)
    }
    // Close path: straight to Shutdown with traffic in flight, no final drain.
    NetSubsystem::Instance().Shutdown();
    CHECK(true, "traffic-then-shutdown: did not crash");
}

// SendBuffer move-only contract + AbortSend release + Send overflow guard (API-safety
// hardening). AcquireSend is the only producer; we observe consume-on-move and that
// misuse (re-consume / overflow) is a safe no-op rather than a double-release/overrun.
static void test_send_buffer_lifecycle() {
    NetServices net{};
    NetServicesImpl::Init(net);
    NetSubsystem::Instance().Init();

    SendBuffer sb = net.AcquireSend(64);
    CHECK(sb.data != nullptr, "lifecycle: AcquireSend gives data");
    CHECK(sb.cap == 64, "lifecycle: cap matches request");

    // Move nulls the source (move-only consume semantics).
    SendBuffer moved = std::move(sb);
    CHECK(sb.data == nullptr, "lifecycle: moved-from SendBuffer is null");
    CHECK(moved.data != nullptr, "lifecycle: moved-to retains buffer");

    // AbortSend consumes (releases the block) + nulls the local; re-abort is a safe no-op.
    net.AbortSend(std::move(moved));
    CHECK(moved.data == nullptr, "lifecycle: AbortSend consumed the buffer");
    net.AbortSend(std::move(moved));   // no double-release

    // Send overflow guard: payloadLen > cap → refused + released, returns false (no overrun).
    SendBuffer over = net.AcquireSend(16);
    CHECK(over.data != nullptr, "lifecycle: acquire for overflow test");
    const bool sent = net.Send(NetHandle::Invalid, kNetConnInvalid, std::move(over), 16u + 100u);
    CHECK(!sent, "lifecycle: Send refuses payloadLen > cap");
    CHECK(over.data == nullptr, "lifecycle: refused Send still consumed the buffer");

    // Acquire/abort more times than the pool has blocks → blocks must recycle (no crash/leak).
    for (int i = 0; i < 3000; ++i) {
        SendBuffer s = net.AcquireSend(32);
        CHECK(s.data != nullptr, "lifecycle: churn acquire");
        net.AbortSend(std::move(s));
    }

    NetSubsystem::Instance().Shutdown();
}

int main() {
    test_net_types();
    test_mpsc_basic();
    test_mpsc_concurrent();
    test_mpsc_mpmc();
    test_mpsc_single_consumer();
    test_pool_basic();
    test_pool_exhaustion();
    test_pool_peak();
    test_pool_concurrent();
    test_netsub_roundtrip();
    test_release_game_resident();
    test_netsub_shutdown_while_connected();
    test_netsub_traffic_then_shutdown();
    test_send_buffer_lifecycle();
    if (g_Failures == 0) { std::printf("All net tests passed.\n"); return 0; }
    std::printf("%d net test(s) FAILED.\n", g_Failures);
    return 1;
}
